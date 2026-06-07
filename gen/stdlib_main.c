/* Minimal entry for the stdlib-ROM generator tool. The spec and all the
 * generator logic live in Aether (gen/genengine/module.ae); this just
 * forwards argc/argv to build_stdlib_main. Replaces mqjs_stdlib.c. */
int build_stdlib_main(int argc, char **argv); /* gen/genengine/module.ae */

int main(int argc, char **argv)
{
    return build_stdlib_main(argc, argv);
}
