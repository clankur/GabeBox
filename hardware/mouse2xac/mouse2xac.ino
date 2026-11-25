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
 *
 * The package decodes the stick with the contract in README.md. Keep the
 * constants below and that contract in step.
 */

// Set to 1 to keep the CDC serial port for debugging. The XAC tolerated it in
// upstream testing. With it off, enter the bootloader with the board's buttons
// before uploading.
#define USB_DEBUG 0

#include <pico/mutex.h>
#include <pico/time.h>
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"

#if !defined(PIN_USB_HOST_DP) || !defined(PIN_5V_EN)
#error "Build for the Adafruit Feather RP2040 USB Host board."
#endif

// --- Encoding contract (mirrored in README.md) ------------------------------

// Each stick value is held for this long. Set it at or above the interval the
// console's controller readings actually update at, as measured by the
// controller pointer probe, so no value is skipped.
constexpr uint32_t OUTPUT_PERIOD_US = 8000;
// Mouse speed, in counts per second, that maps to full stick deflection.
// 8000 counts/s is 5 in/s on a 1600 CPI mouse.
constexpr float FULL_SCALE_COUNTS_PER_SECOND = 8000.0f;
// Motion beyond full deflection is delivered over later periods, at most this
// many periods' worth. Anything above it is dropped so a hard flick cannot leave
// the cursor drifting for long after the hand stops.
constexpr float BACKLOG_PERIODS = 6.0f;

constexpr int STICK_CENTER = 512;
constexpr int STICK_REACH = 511;  // 1..1023
constexpr float COUNTS_PER_STICK_UNIT =
    FULL_SCALE_COUNTS_PER_SECOND * (OUTPUT_PERIOD_US / 1e6f) / STICK_REACH;
constexpr float BACKLOG_COUNTS = STICK_REACH * COUNTS_PER_STICK_UNIT * BACKLOG_PERIODS;

// The XAC reads only the first 8 joystick buttons. Mouse buttons use bits 0-4
// (left, right, middle, back, forward); the wheel takes bits 5 and 6.
constexpr uint8_t MOUSE_BUTTONS = 0x1F;
constexpr uint8_t WHEEL_UP_BUTTON = 0x20;
constexpr uint8_t WHEEL_DOWN_BUTTON = 0x40;
constexpr int32_t WHEEL_PENDING_LIMIT = 16;

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

// --- Core 0: joystick device ------------------------------------------------

void setup()
{
#if USB_DEBUG
  Serial.begin(115200);
#else
  Serial.end();
#endif
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  joystick.setPollInterval(2);
  joystick.setReportDescriptor(joystick_descriptor, sizeof(joystick_descriptor));
  joystick.begin();
  // The HID interface only appears after re-enumeration if the device mounted first.
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

static int deflect(float &pending)
{
  pending = constrain(pending, -BACKLOG_COUNTS, BACKLOG_COUNTS);
  int units = constrain(lroundf(pending / COUNTS_PER_STICK_UNIT), -STICK_REACH, STICK_REACH);
  // Keep the fractional remainder so slow motion still adds up to whole units.
  pending -= units * COUNTS_PER_STICK_UNIT;
  return STICK_CENTER + units;
}

void loop()
{
  static uint64_t next_output = 0;
  static float pending_x = 0.0f;
  static float pending_y = 0.0f;
  static int32_t wheel_pending = 0;
  static bool wheel_pressed = false;

  uint64_t now = time_us_64();
  if (now < next_output) {
    return;
  }
  if (!TinyUSBDevice.mounted() || !joystick.ready()) {
    return;
  }
  // Late periods do not bunch up: the next one is a full period from now.
  next_output = now + OUTPUT_PERIOD_US;

  mutex_enter_blocking(&motion_lock);
  int32_t dx = motion.dx;
  int32_t dy = motion.dy;
  int32_t wheel = motion.wheel;
  uint8_t buttons = motion.latched;
  motion.dx = motion.dy = motion.wheel = 0;
  motion.latched = motion.held;
  mutex_exit(&motion_lock);

  pending_x += dx;
  pending_y += dy;
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

  JoystickReport report = {};
  report.x = deflect(pending_x);
  report.y = deflect(pending_y);
  report.hat = HAT_NULL;
  report.twist = 0x80;
  report.slider = 0x80;
  report.buttons_a = (buttons & MOUSE_BUTTONS) | wheel_button;
  joystick.sendReport(0, &report, sizeof(report));
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
  for (MouseSlot &other : mice) {
    if (other.address != 0) {
      return;
    }
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
    }
  }
  if (!tuh_hid_receive_report(address, instance)) {
    Serial.println("Cannot request a mouse report");
  }
}
