#include <stddef.h>
#include <stdint.h>

/* Independent, bounded re-expression of the documented byte-to-hex contract. */
int encode_hex(const uint8_t *input, size_t input_size, char *output,
               size_t output_capacity) {
  static const char digits[] = "0123456789abcdef";
  if (output == NULL || output_capacity < 1 ||
      (input_size != 0 && input == NULL))
    return 0;
  if (input_size > (output_capacity - 1) / 2)
    return 0;
  for (size_t i = 0; i < input_size; ++i) {
    output[2 * i] = digits[input[i] >> 4];
    output[2 * i + 1] = digits[input[i] & 0x0f];
  }
  output[2 * input_size] = '\0';
  return 1;
}
