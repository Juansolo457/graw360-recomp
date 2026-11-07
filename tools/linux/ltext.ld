/* ThinLTO (tools/build-linux.sh, GRAW_LTO): the translated code is compiled with -mcmodel=large
   into .ltext, and LTO's per-function sections arrive as .ltext.<function>, which lld 20 doesn't
   merge: 100,000+ output sections, past what the section index fields hold. One .ltext again. */
SECTIONS {
  .ltext : { *(.ltext .ltext.*) }
} INSERT AFTER .text;
