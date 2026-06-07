/* Entry for the example demo's stdlib-ROM generator. The spec (standard
 * stdlib + Rectangle/FilledRectangle) and all generator logic are in
 * Aether (gen/genengine/module.ae). Replaces example_stdlib.c. */
int build_example_main(int argc, char **argv); /* gen/genengine/module.ae */

int main(int argc, char **argv)
{
    return build_example_main(argc, argv);
}
