/*
 * USB mouse to Xbox Adaptive Controller joystick, tuned as a cursor source.
 *
 * Forked from touchgadget/mouse2xac at 081f1cafb8c13df16a23d0ed2052255fcf8d7fac
 * (MIT, Copyright (c) 2023 touchgadgetdev@gmail.com, see LICENSE). The joystick
 * report layout and descriptor come from touchgadget/flight_stick_tinyusb at
 * ff67cad319bc67ad499efb8403c14bf4c1f5d963 (MIT, same author). Upstream derives
 * from the Adafruit TinyUSB dual-role examples (MIT, Copyright (c) 2019 Ha Thach
 * for Adafruit Industries).
 *
 * Board: Adafruit Feather RP2040 USB Host. Core 1 runs the mouse host on
 * Pico-PIO-USB; core 0 runs the joystick device the XAC reads. The CPU must run
 * at 120 or 240 MHz for PIO USB.
 *
 * Changes from upstream, each addressing a way the upstream sketch loses motion
 * when the stick drives a cursor:
 *
 * - Motion is accumulated across every mouse report instead of keeping only
 *   the latest one, so reports that arrive between joystick reports are kept.
 * - The stick is written once per fixed output period and holds that value for
 *   the whole period. Deflection encodes velocity on a fixed scale, so one stick
 *   unit is a known number of mouse counts per second whatever the mouse's
 *   report rate. Motion beyond full deflection carries into later periods, up
 *   to a bounded backlog, instead of being clipped.
 * - The wheel becomes two joystick buttons that pulse once per detent.
 * - A press shorter than one output period is still reported for one period.
 * - The hat switch rests at its null position rather than at 0, which is "up".
 * - The encoding settings are adjustable at runtime over the joystick's vendor
 *   feature report and persist in flash. See "Settings" and "Config protocol"
 *   in README.md.
 *
 * The package decodes the stick with the contract in README.md. Keep the
 * setting defaults below and that contract in step, and keep the register and
 * command numbers in step with tuner/protocol.js.
 */

// Set to 1 to keep the CDC serial port for debugging. The XAC tolerated it in
// upstream testing. With it off, enter the bootloader with the board's buttons
// before uploading.
#define USB_DEBUG 0

#include <EEPROM.h>
#include <hardware/sync.h>
#include <pico/mutex.h>
#include <pico/time.h>
#include <stddef.h>
#include <string.h>
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"

#if !defined(PIN_USB_HOST_DP) || !defined(PIN_5V_EN)
#error "Build for the Adafruit Feather RP2040 USB Host board."
#endif

constexpr uint32_t FIRMWARE_VERSION = 0x000100;  // 0.1.0 as major, minor, patch bytes
constexpr uint32_t PROTOCOL_VERSION = 1;

// --- Settings (mirrored in README.md and tuner/protocol.js) ----------------

// Setting ids double as config-protocol register numbers.
constexpr uint8_t SETTING_OUTPUT_PERIOD_US = 0x01;
constexpr uint8_t SETTING_FULL_SCALE_COUNTS_PER_SECOND = 0x02;
constexpr uint8_t SETTING_BACKLOG_PERIODS = 0x03;
constexpr uint8_t SETTING_INVERT_X = 0x04;
constexpr uint8_t SETTING_INVERT_Y = 0x05;
constexpr uint8_t SETTING_WHEEL_ENABLED = 0x06;
constexpr uint8_t SETTING_SLOTS = 7;  // slot 0 is unused

struct SettingSpec {
  uint32_t initial;
  uint32_t min;
  uint32_t max;
};

// Each stick value is held for one output period. Set it at or above the
// interval the console's controller readings actually update at, as measured
// by the controller pointer probe, so no value is skipped. The joystick
// endpoint is polled every 2 ms, so shorter periods cannot be delivered.
//
// Full scale is the mouse speed, in counts per second, that maps to full stick
// deflection. 8000 counts/s is 5 in/s on a 1600 CPI mouse.
//
// Motion beyond full deflection is delivered over later periods, at most the
// backlog's worth of full-deflection periods including the current one.
// Anything above it is dropped so a hard flick cannot leave the cursor drifting
// for long after the hand stops.
constexpr SettingSpec SETTING_SPECS[SETTING_SLOTS] = {
  // initial, min, max
  {0, 0, 0},              // 0x00 unused
  {8000, 2000, 100000},   // 0x01 output period, microseconds
  {8000, 1, 2000000},     // 0x02 full scale, mouse counts per second
  {6, 1, 64},             // 0x03 backlog, full-deflection periods
  {0, 0, 1},              // 0x04 invert X
  {0, 0, 1},              // 0x05 invert Y
  {1, 0, 1},              // 0x06 wheel buttons enabled
};

struct Settings {
  uint32_t value[SETTING_SLOTS];
};

constexpr int STICK_CENTER = 512;
constexpr int STICK_REACH = 511;  // 1..1023

// Values derived from the settings that the output loop uses.
struct Encoding {
  uint32_t output_period_us;
  float counts_per_unit;  // mouse counts per stick unit per period
  float backlog_counts;   // largest pending motion kept, in counts
  bool invert_x;
  bool invert_y;
  bool wheel_enabled;
};

// The XAC reads only the first 8 joystick buttons. Mouse buttons use bits 0-4
// (left, right, middle, back, forward); the wheel takes bits 5 and 6.
constexpr uint8_t MOUSE_BUTTONS = 0x1F;
constexpr uint8_t WHEEL_UP_BUTTON = 0x20;
constexpr uint8_t WHEEL_DOWN_BUTTON = 0x40;
constexpr int32_t WHEEL_PENDING_LIMIT = 16;

// --- Config protocol (mirrored in README.md and tuner/protocol.js) ---------

// SET_FEATURE {command, value bits 0-7, 8-15, 16-23}
constexpr uint8_t CONFIG_CMD_SELECT = 0x00;            // value: register for the next GET
constexpr uint8_t CONFIG_CMD_SAVE = 0x01;              // write the settings to flash
constexpr uint8_t CONFIG_CMD_RESTORE_DEFAULTS = 0x02;  // defaults in RAM; SAVE keeps them
constexpr uint8_t CONFIG_CMD_RELOAD = 0x03;            // discard unsaved changes
constexpr uint8_t CONFIG_CMD_CLEAR_COUNTERS = 0x04;    // zero the report and clip counters
constexpr uint8_t CONFIG_CMD_WRITE = 0x80;             // | setting id; value: new setting

// GET_FEATURE returns {selected register, value bits 0-23}. Registers
// 0x01-0x06 are the settings; these are read-only.
constexpr uint8_t CONFIG_REG_FIRMWARE_VERSION = 0x10;
constexpr uint8_t CONFIG_REG_PROTOCOL_VERSION = 0x11;
constexpr uint8_t CONFIG_REG_MOUSE_REPORTS = 0x12;
constexpr uint8_t CONFIG_REG_MICE_CONNECTED = 0x13;
constexpr uint8_t CONFIG_REG_BACKLOG_CLIPPED = 0x14;
constexpr uint8_t CONFIG_REG_STATUS = 0x15;
constexpr uint8_t CONFIG_REG_FLASH_SAVES = 0x16;
constexpr uint8_t CONFIG_REG_ERROR = 0xFF;

constexpr uint32_t CONFIG_STATUS_FLASH_VALID = 0x01;  // flash held valid settings at boot or was saved
constexpr uint32_t CONFIG_STATUS_SAVE_PENDING = 0x02;
constexpr uint32_t CONFIG_STATUS_UNSAVED = 0x04;      // RAM settings differ from flash
constexpr uint32_t CONFIG_STATUS_SAVE_FAILED = 0x08;

constexpr uint32_t CONFIG_ERROR_UNKNOWN_COMMAND = 1;
constexpr uint32_t CONFIG_ERROR_UNKNOWN_REGISTER = 2;
constexpr uint32_t CONFIG_ERROR_OUT_OF_RANGE = 3;

constexpr uint16_t CONFIG_REPORT_SIZE = 4;
constexpr uint32_t VALUE_MASK = 0xFFFFFF;

// --- Joystick device (flight_stick_tinyusb layout) --------------------------

typedef struct __attribute__((packed)) {
  uint32_t x : 10;     // 0..512..1023
  uint32_t y : 10;     // 0..512..1023, larger is down
  uint32_t hat : 4;    // 0..7, 0xF is null
  uint32_t twist : 8;  // 0..128..255
  uint8_t buttons_a;   // buttons 1-8
  uint8_t slider;      // 0..255
  uint8_t buttons_b;   // buttons 9-12 in the low nibble
} JoystickReport;

constexpr uint8_t HAT_NULL = 0xF;

uint8_t const joystick_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
  0x09, 0x04,        // Usage (Joystick)
  0xA1, 0x01,        // Collection (Application)
  0xA1, 0x02,        //   Collection (Logical)
  0x95, 0x02,        //     Report Count (2)
  0x75, 0x0A,        //     Report Size (10)
  0x15, 0x00,        //     Logical Minimum (0)
  0x26, 0xFF, 0x03,  //     Logical Maximum (1023)
  0x35, 0x00,        //     Physical Minimum (0)
  0x46, 0xFF, 0x03,  //     Physical Maximum (1023)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x81, 0x02,        //     Input (Data,Var,Abs)
  0x75, 0x04,        //     Report Size (4)
  0x95, 0x01,        //     Report Count (1)
  0x25, 0x07,        //     Logical Maximum (7)
  0x46, 0x3B, 0x01,  //     Physical Maximum (315)
  0x66, 0x14, 0x00,  //     Unit (English Rotation)
  0x09, 0x39,        //     Usage (Hat switch)
  0x81, 0x42,        //     Input (Data,Var,Abs,Null State)
  0x65, 0x00,        //     Unit (None)
  0x75, 0x08,        //     Report Size (8)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x46, 0xFF, 0x00,  //     Physical Maximum (255)
  0x09, 0x35,        //     Usage (Rz)
  0x81, 0x02,        //     Input (Data,Var,Abs)
  0xA4,              //     Push
  0x95, 0x08,        //       Report Count (8)
  0x75, 0x01,        //       Report Size (1)
  0x25, 0x01,        //       Logical Maximum (1)
  0x45, 0x01,        //       Physical Maximum (1)
  0x05, 0x09,        //       Usage Page (Button)
  0x19, 0x01,        //       Usage Minimum (1)
  0x29, 0x08,        //       Usage Maximum (8)
  0x81, 0x02,        //       Input (Data,Var,Abs)
  0xB4,              //     Pop
  0x09, 0x36,        //     Usage (Slider)
  0x81, 0x02,        //     Input (Data,Var,Abs)
  0x95, 0x04,        //     Report Count (4)
  0x75, 0x01,        //     Report Size (1)
  0x25, 0x01,        //     Logical Maximum (1)
  0x45, 0x01,        //     Physical Maximum (1)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x09,        //     Usage Minimum (9)
  0x29, 0x0C,        //     Usage Maximum (12)
  0x81, 0x02,        //     Input (Data,Var,Abs)
  0x95, 0x04,        //     Report Count (4)
  0x81, 0x01,        //     Input (Const)
  0xC0,              //   End Collection
  0xA1, 0x02,        //   Collection (Logical)
  0x95, 0x04,        //     Report Count (4)
  0x75, 0x08,        //     Report Size (8)
  0x26, 0xFF, 0x00,  //     Logical Maximum (255)
  0x46, 0xFF, 0x00,  //     Physical Maximum (255)
  0x06, 0x00, 0xFF,  //     Usage Page (Vendor Defined 0xFF00)
  0x09, 0x01,        //     Usage (0x01)
  0xB1, 0x02,        //     Feature (Data,Var,Abs)
  0xC0,              //   End Collection
  0xC0,              // End Collection
};

Adafruit_USBD_HID joystick;
Adafruit_USBH_Host USBHost;

// --- Motion shared between the host core and the device core ---------------

struct Motion {
  int32_t dx = 0;
  int32_t dy = 0;
  int32_t wheel = 0;
  uint8_t held = 0;     // buttons down in the latest report
  uint8_t latched = 0;  // buttons down in any report since the last drain
};

auto_init_mutex(motion_lock);
Motion motion;

struct MouseSlot {
  uint8_t address = 0;
  uint8_t instance = 0;
  bool skip_report_id = false;
};

// Only core 1 touches the slot table.
constexpr size_t MOUSE_SLOTS = 4;
MouseSlot mice[MOUSE_SLOTS];

// --- Counters for the config channel ----------------------------------------

// Each counter has one writer. 32-bit loads and stores are atomic on both cores,
// so the config callback reads them without the motion lock.
volatile uint32_t mouse_reports = 0;          // core 1
volatile uint32_t mice_connected = 0;         // core 1
volatile uint32_t backlog_clipped_counts = 0; // core 0 output loop

// --- Settings state ----------------------------------------------------------

// The config callbacks run inside TinyUSB's device task, which the Adafruit
// TinyUSB port runs from a core 0 interrupt as well as from thread context, so
// a callback can preempt loop() at any point. The rules:
//
// - Only the callbacks write `settings` and the fields marked "callback".
// - loop() reads `settings` only with interrupts disabled, and turns it into
//   `encoding` when settings_generation moves.
// - Flash is written only from loop(), never from a callback, because a flash
//   write stalls both cores.
// - The callbacks never take motion_lock, which loop() can hold.
constexpr uint32_t SETTINGS_MAGIC = 0x4358324D;  // "M2XC"
constexpr uint16_t SETTINGS_VERSION = 1;
constexpr size_t SETTINGS_EEPROM_BYTES = 256;

struct StoredSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  Settings settings;
  uint32_t checksum;  // FNV-1a over every byte before this field
};
static_assert(sizeof(StoredSettings) <= SETTINGS_EEPROM_BYTES, "settings record exceeds the EEPROM size");

Settings settings;                          // live settings, written by the callbacks
Settings flash_settings;                    // what flash holds, written by setup() and loop()
volatile uint32_t settings_generation = 0;  // callback, bumped on every change
volatile bool save_requested = false;       // callback sets, loop() clears
volatile bool save_in_progress = false;     // loop()
volatile bool flash_valid = false;          // loop()
volatile bool save_failed = false;          // loop()
volatile uint32_t flash_saves = 0;          // loop()

uint8_t selected_register = CONFIG_REG_PROTOCOL_VERSION;  // callback
uint32_t last_error = 0;                                   // callback
uint32_t mouse_reports_base = 0;                           // callback
uint32_t backlog_clipped_base = 0;                         // callback

// Only loop() touches these.
Encoding encoding;
uint32_t applied_generation = 0;

static Settings default_settings()
{
  Settings defaults = {};
  for (uint8_t id = 0; id < SETTING_SLOTS; id++) {
    defaults.value[id] = SETTING_SPECS[id].initial;
  }
  return defaults;
}

static bool is_setting(uint8_t id)
{
  return id >= 1 && id < SETTING_SLOTS;
}

static bool setting_in_range(uint8_t id, uint32_t value)
{
  return is_setting(id) && value >= SETTING_SPECS[id].min && value <= SETTING_SPECS[id].max;
}

static Encoding encoding_for(const Settings &s)
{
  Encoding e;
  e.output_period_us = s.value[SETTING_OUTPUT_PERIOD_US];
  e.counts_per_unit = (float)s.value[SETTING_FULL_SCALE_COUNTS_PER_SECOND] *
      ((float)s.value[SETTING_OUTPUT_PERIOD_US] / 1e6f) / STICK_REACH;
  e.backlog_counts = STICK_REACH * e.counts_per_unit * (float)s.value[SETTING_BACKLOG_PERIODS];
  e.invert_x = s.value[SETTING_INVERT_X] != 0;
  e.invert_y = s.value[SETTING_INVERT_Y] != 0;
  e.wheel_enabled = s.value[SETTING_WHEEL_ENABLED] != 0;
  return e;
}

static uint32_t settings_checksum(const StoredSettings &record)
{
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(StoredSettings, checksum); i++) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

static bool stored_settings_valid(const StoredSettings &record)
{
  if (record.magic != SETTINGS_MAGIC || record.version != SETTINGS_VERSION ||
      record.size != sizeof(StoredSettings) || record.checksum != settings_checksum(record)) {
    return false;
  }
  for (uint8_t id = 1; id < SETTING_SLOTS; id++) {
    if (!setting_in_range(id, record.settings.value[id])) {
      return false;
    }
  }
  return true;
}

// Runs in setup() before the config callbacks are registered.
static void load_settings()
{
  EEPROM.begin(SETTINGS_EEPROM_BYTES);
  StoredSettings record = {};
  EEPROM.get(0, record);
  flash_valid = stored_settings_valid(record);
  flash_settings = flash_valid ? record.settings : default_settings();
  settings = flash_settings;
  encoding = encoding_for(settings);
  applied_generation = settings_generation;
}

// loop(): pick up settings the callbacks changed.
static void apply_settings_changes()
{
  if (settings_generation == applied_generation) {
    return;
  }
  uint32_t irq = save_and_disable_interrupts();
  Settings snapshot = settings;
  applied_generation = settings_generation;
  restore_interrupts(irq);
  encoding = encoding_for(snapshot);
}

// loop(): write the live settings to flash. EEPROM.commit() pauses core 1, so
// the mouse host stops for the length of a sector erase and program.
static void save_settings()
{
  StoredSettings record = {};
  uint32_t irq = save_and_disable_interrupts();
  save_requested = false;
  save_in_progress = true;
  record.settings = settings;
  restore_interrupts(irq);

  record.magic = SETTINGS_MAGIC;
  record.version = SETTINGS_VERSION;
  record.size = sizeof(StoredSettings);
  record.checksum = settings_checksum(record);
  EEPROM.put(0, record);
  bool ok = EEPROM.commit();

  irq = save_and_disable_interrupts();
  if (ok) {
    flash_settings = record.settings;
    flash_valid = true;
    flash_saves = flash_saves + 1;
  }
  save_failed = !ok;
  save_in_progress = false;
  restore_interrupts(irq);
}

// --- Config channel (vendor feature report, no report ID) --------------------

static uint32_t config_status()
{
  uint32_t flags = 0;
  if (flash_valid) {
    flags |= CONFIG_STATUS_FLASH_VALID;
  }
  if (save_requested || save_in_progress) {
    flags |= CONFIG_STATUS_SAVE_PENDING;
  }
  if (memcmp(&settings, &flash_settings, sizeof(Settings)) != 0) {
    flags |= CONFIG_STATUS_UNSAVED;
  }
  if (save_failed) {
    flags |= CONFIG_STATUS_SAVE_FAILED;
  }
  return flags;
}

static bool config_register_readable(uint32_t id)
{
  return (id >= 1 && id < SETTING_SLOTS) || (id >= CONFIG_REG_FIRMWARE_VERSION && id <= CONFIG_REG_FLASH_SAVES) ||
      id == CONFIG_REG_ERROR;
}

static uint32_t config_register_value(uint8_t id)
{
  if (is_setting(id)) {
    return settings.value[id];
  }
  switch (id) {
  case CONFIG_REG_FIRMWARE_VERSION:
    return FIRMWARE_VERSION;
  case CONFIG_REG_PROTOCOL_VERSION:
    return PROTOCOL_VERSION;
  case CONFIG_REG_MOUSE_REPORTS:
    return (mouse_reports - mouse_reports_base) & VALUE_MASK;  // wraps
  case CONFIG_REG_MICE_CONNECTED:
    return mice_connected;
  case CONFIG_REG_BACKLOG_CLIPPED: {
    uint32_t clipped = backlog_clipped_counts - backlog_clipped_base;
    return clipped > VALUE_MASK ? VALUE_MASK : clipped;  // saturates
  }
  case CONFIG_REG_STATUS:
    return config_status();
  case CONFIG_REG_FLASH_SAVES:
    return flash_saves & VALUE_MASK;
  case CONFIG_REG_ERROR:
    return last_error;
  default:
    return 0;
  }
}

static void settings_changed()
{
  settings_generation = settings_generation + 1;
}

static void config_reject(uint32_t code, uint8_t command, uint32_t value)
{
  last_error = code | ((uint32_t)command << 8) | ((value & 0xFF) << 16);
  selected_register = CONFIG_REG_ERROR;
}

static void config_run_command(uint8_t command, uint32_t value)
{
  if (command & CONFIG_CMD_WRITE) {
    uint8_t id = command & 0x7F;
    if (!is_setting(id)) {
      config_reject(CONFIG_ERROR_UNKNOWN_REGISTER, command, id);
    } else if (!setting_in_range(id, value)) {
      config_reject(CONFIG_ERROR_OUT_OF_RANGE, command, value);
    } else {
      settings.value[id] = value;
      settings_changed();
      selected_register = id;
    }
    return;
  }
  switch (command) {
  case CONFIG_CMD_SELECT:
    if (config_register_readable(value)) {
      selected_register = (uint8_t)value;
    } else {
      config_reject(CONFIG_ERROR_UNKNOWN_REGISTER, command, value);
    }
    break;
  case CONFIG_CMD_SAVE:
    save_requested = true;
    selected_register = CONFIG_REG_STATUS;
    break;
  case CONFIG_CMD_RESTORE_DEFAULTS:
    settings = default_settings();
    settings_changed();
    selected_register = CONFIG_REG_STATUS;
    break;
  case CONFIG_CMD_RELOAD:
    settings = flash_settings;
    settings_changed();
    selected_register = CONFIG_REG_STATUS;
    break;
  case CONFIG_CMD_CLEAR_COUNTERS:
    mouse_reports_base = mouse_reports;
    backlog_clipped_base = backlog_clipped_counts;
    selected_register = CONFIG_REG_MOUSE_REPORTS;
    break;
  default:
    config_reject(CONFIG_ERROR_UNKNOWN_COMMAND, command, value);
    break;
  }
}

static uint16_t config_get_report(uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
  if (report_id != 0 || report_type != HID_REPORT_TYPE_FEATURE || reqlen < CONFIG_REPORT_SIZE) {
    return 0;  // stalls the request
  }
  uint32_t value = config_register_value(selected_register) & VALUE_MASK;
  buffer[0] = selected_register;
  buffer[1] = value & 0xFF;
  buffer[2] = (value >> 8) & 0xFF;
  buffer[3] = (value >> 16) & 0xFF;
  return CONFIG_REPORT_SIZE;
}

static void config_set_report(uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
  if (report_id != 0 || report_type != HID_REPORT_TYPE_FEATURE) {
    return;
  }
  // Tolerate a host that passes the unnumbered report's zero ID through.
  if (bufsize == CONFIG_REPORT_SIZE + 1 && buffer[0] == 0) {
    buffer++;
    bufsize--;
  }
  if (bufsize < CONFIG_REPORT_SIZE) {
    return;
  }
  uint32_t value = buffer[1] | ((uint32_t)buffer[2] << 8) | ((uint32_t)buffer[3] << 16);
  config_run_command(buffer[0], value);
}

// --- Core 0: joystick device ------------------------------------------------

void setup()
{
#if USB_DEBUG
  Serial.begin(115200);
#else
  Serial.end();
#endif
  load_settings();
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  joystick.setPollInterval(2);
  joystick.setReportDescriptor(joystick_descriptor, sizeof(joystick_descriptor));
  joystick.setReportCallback(config_get_report, config_set_report);
  joystick.begin();
  // The HID interface only appears after re-enumeration if the device mounted first.
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

static float clamp_float(float value, float low, float high)
{
  return value < low ? low : (value > high ? high : value);
}

static int deflect(float &pending, float &dropped)
{
  float limited = clamp_float(pending, -encoding.backlog_counts, encoding.backlog_counts);
  dropped += fabsf(pending - limited);
  pending = limited;
  long units = lroundf(pending / encoding.counts_per_unit);
  if (units > STICK_REACH) {
    units = STICK_REACH;
  } else if (units < -STICK_REACH) {
    units = -STICK_REACH;
  }
  // Keep the fractional remainder so slow motion still adds up to whole units.
  pending -= (int)units * encoding.counts_per_unit;
  return STICK_CENTER + (int)units;
}

void loop()
{
  static uint64_t next_output = 0;
  static float pending_x = 0.0f;
  static float pending_y = 0.0f;
  static int32_t wheel_pending = 0;
  static bool wheel_pressed = false;
  static float clipped_fraction = 0.0f;

  apply_settings_changes();
  if (save_requested) {
    save_settings();
  }

  uint64_t now = time_us_64();
  if (now < next_output) {
    return;
  }
  if (!TinyUSBDevice.mounted() || !joystick.ready()) {
    return;
  }
  // Late periods do not bunch up: the next one is a full period from now.
  next_output = now + encoding.output_period_us;

  mutex_enter_blocking(&motion_lock);
  int32_t dx = motion.dx;
  int32_t dy = motion.dy;
  int32_t wheel = motion.wheel;
  uint8_t buttons = motion.latched;
  motion.dx = motion.dy = motion.wheel = 0;
  motion.latched = motion.held;
  mutex_exit(&motion_lock);

  pending_x += encoding.invert_x ? -dx : dx;
  pending_y += encoding.invert_y ? -dy : dy;
  if (!encoding.wheel_enabled) {
    wheel = 0;
    wheel_pending = 0;
  }
  wheel_pending = constrain(wheel_pending + wheel, -WHEEL_PENDING_LIMIT, WHEEL_PENDING_LIMIT);

  // One detent is one period pressed then one period released, so the reader
  // sees an edge for every detent.
  uint8_t wheel_button = 0;
  if (wheel_pressed) {
    wheel_pressed = false;
  } else if (wheel_pending != 0) {
    wheel_button = wheel_pending > 0 ? WHEEL_UP_BUTTON : WHEEL_DOWN_BUTTON;
    wheel_pending += wheel_pending > 0 ? -1 : 1;
    wheel_pressed = true;
  }

  float dropped = 0.0f;
  JoystickReport report = {};
  report.x = deflect(pending_x, dropped);
  report.y = deflect(pending_y, dropped);
  report.hat = HAT_NULL;
  report.twist = 0x80;
  report.slider = 0x80;
  report.buttons_a = (buttons & MOUSE_BUTTONS) | wheel_button;
  joystick.sendReport(0, &report, sizeof(report));

  // Count whole clipped counts for the config channel.
  clipped_fraction += dropped;
  if (clipped_fraction >= 1.0f) {
    uint32_t whole = (uint32_t)clipped_fraction;
    clipped_fraction -= (float)whole;
    backlog_clipped_counts = backlog_clipped_counts + whole;
  }
}

// --- Core 1: mouse host -----------------------------------------------------

void setup1()
{
  uint32_t cpu_hz = clock_get_hz(clk_sys);
  if (cpu_hz != 120000000UL && cpu_hz != 240000000UL) {
    Serial.printf("PIO USB needs a 120 or 240 MHz CPU clock, not %lu Hz\r\n", cpu_hz);
    while (true) {
      delay(1);
    }
  }
  pinMode(PIN_5V_EN, OUTPUT);
  digitalWrite(PIN_5V_EN, HIGH);

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = PIN_USB_HOST_DP;
  USBHost.configure_pio_usb(1, &pio_cfg);
  // Starting the host on core 1 keeps PIO bit-banging off the device core.
  USBHost.begin(1);
}

void loop1()
{
  USBHost.task();
}

static MouseSlot *find_mouse(uint8_t address, uint8_t instance)
{
  for (MouseSlot &slot : mice) {
    if (slot.address == address && slot.instance == instance) {
      return &slot;
    }
  }
  return nullptr;
}

static uint32_t count_mice()
{
  uint32_t count = 0;
  for (const MouseSlot &slot : mice) {
    if (slot.address != 0) {
      count++;
    }
  }
  return count;
}

void tuh_hid_mount_cb(uint8_t address, uint8_t instance, uint8_t const *descriptor, uint16_t length)
{
  uint8_t const interface_protocol = tuh_hid_interface_protocol(address, instance);
  uint8_t const protocol_mode = tuh_hid_get_protocol(address, instance);
  tuh_hid_report_info_t info[8];
  uint8_t count = tuh_hid_parse_report_descriptor(info, 8, descriptor, length);

  bool is_mouse = interface_protocol == HID_ITF_PROTOCOL_MOUSE;
  bool skip_report_id = false;
  for (uint8_t i = 0; i < count; i++) {
    if (info[i].usage_page == HID_USAGE_PAGE_DESKTOP && info[i].usage == HID_USAGE_DESKTOP_MOUSE) {
      is_mouse = true;
      // A boot-protocol report never carries a report ID.
      skip_report_id = info[i].report_id != 0 &&
          !(interface_protocol == HID_ITF_PROTOCOL_MOUSE && protocol_mode == HID_PROTOCOL_BOOT);
      break;
    }
  }
  Serial.printf("HID %u.%u mounted, mouse=%d skip_id=%d\r\n", address, instance, is_mouse, skip_report_id);
  if (!is_mouse) {
    return;
  }
  MouseSlot *slot = find_mouse(0, 0);
  if (!slot) {
    Serial.println("No free mouse slot");
    return;
  }
  slot->address = address;
  slot->instance = instance;
  slot->skip_report_id = skip_report_id;
  mice_connected = count_mice();
  if (!tuh_hid_receive_report(address, instance)) {
    Serial.println("Cannot request a mouse report");
  }
}

void tuh_hid_umount_cb(uint8_t address, uint8_t instance)
{
  MouseSlot *slot = find_mouse(address, instance);
  if (!slot) {
    return;
  }
  *slot = MouseSlot();
  uint32_t remaining = count_mice();
  mice_connected = remaining;
  if (remaining != 0) {
    return;
  }
  // Release the buttons when the last mouse goes away.
  mutex_enter_blocking(&motion_lock);
  motion.held = 0;
  mutex_exit(&motion_lock);
}

void tuh_hid_report_received_cb(uint8_t address, uint8_t instance, uint8_t const *report, uint16_t length)
{
  MouseSlot *slot = find_mouse(address, instance);
  if (slot) {
    if (slot->skip_report_id && length > 0) {
      report++;
      length--;
    }
    // Boot-protocol layout: buttons, X, Y, then an optional wheel byte.
    if (length >= 3) {
      uint8_t buttons = report[0];
      int8_t dx = (int8_t)report[1];
      int8_t dy = (int8_t)report[2];
      int8_t wheel = length >= 4 ? (int8_t)report[3] : 0;
      mutex_enter_blocking(&motion_lock);
      motion.dx += dx;
      motion.dy += dy;
      motion.wheel = constrain(motion.wheel + wheel, -WHEEL_PENDING_LIMIT, WHEEL_PENDING_LIMIT);
      motion.held = buttons;
      motion.latched |= buttons;
      mutex_exit(&motion_lock);
      mouse_reports = mouse_reports + 1;
    }
  }
  if (!tuh_hid_receive_report(address, instance)) {
    Serial.println("Cannot request a mouse report");
  }
}
