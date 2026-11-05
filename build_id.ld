/* The GNU build ID, kept in flash where the firmware can read it: crash
 * reports name the build that crashed, and the server finds the same note in
 * the uploaded ELF. */
. = ALIGN(4);
__ps_build_id_start = .;
KEEP(*(.note.gnu.build-id))
__ps_build_id_end = .;
