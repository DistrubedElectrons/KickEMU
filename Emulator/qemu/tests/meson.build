# KICK qtests. Each test is Emulator/qemu/tests/kick-<name>-test.c and is linked into
# tests/qtest by build.sh. Add one line per test, sorted, to keep merges trivial.
#
# Run all of them with:
#   meson test -C Emulator/out/qemu-build --suite qtest-aarch64 'qtest-aarch64/kick-*'

kick_qtests = [
  'kick-ad5675r-test',
  'kick-audio-test',
  'kick-board-test',
  'kick-clock-test',
  'kick-dsim-test',
  'kick-gpu-test',
  'kick-gt911-test',
  'kick-lcdif-test',
  'kick-ocotp-test',
  'kick-pca9534-test',
  'kick-pcf85263-test',
  'kick-pwm-test',
  'kick-sai-test',
  'kick-sd-test',
  'kick-sdma-test',
  'kick-system-test',
  'kick-uart-test',
  'kick-usb-test',
]

qtests_aarch64 += kick_qtests
