# CS_ARCH_MIPS, CS_MODE_MIPS32+CS_MODE_MICRO+CS_MODE_BIG_ENDIAN, None
0x00,0xe6,0x48,0x58 = movz $t1, $a2, $a3
0x00,0xe6,0x48,0x18 = movn $t1, $a2, $a3
0x55,0x26,0x09,0x7b = movt $t1, $a2, $fcc0
0x55,0x26,0x01,0x7b = movf $t1, $a2, $fcc0