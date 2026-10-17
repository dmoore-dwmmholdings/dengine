# Deterministic native-MSL bootstrap embedding; source remains reviewable in shaders/rhi/.
file(READ "${SOURCE}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "/* Generated from accel_instances_metal.metal; do not edit. */\nstatic const unsigned char de__mtl_accel_translation[] = {\n${bytes}0x00};\n")
