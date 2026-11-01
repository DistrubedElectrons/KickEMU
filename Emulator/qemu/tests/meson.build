# KICK qtests. Each test is Emulator/qemu/tests/kick-<name>-test.c and is linked into
# tests/qtest by build.sh. Add one line per test, sorted, to keep merges trivial.
#
# Run all of them with:
#   meson test -C Emulator/out/qemu-build --suite qtest-aarch64 'qtest-aarch64/kick-*'

kick_qtests = [
  'kick-ad5675r-test',
  'kick-dsim-test',
  'kick-gpu-test',
  'kick-gt911-test',
  'kick-lcdif-test',
  'kick-pca9534-test',
  'kick-pcf85263-test',
  'kick-sai-test',
  'kick-sdma-test',
]

qtests_aarch64 += kick_qtests
