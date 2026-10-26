import configparser
from collections import defaultdict
import os
import argparse

def read_build_board_value(ini_file):
    config = configparser.RawConfigParser()
    config.read(ini_file)
    try:
        return config.get('fujinet', 'build_board')
    except (configparser.NoSectionError, configparser.NoOptionError):
        print(f"Error: 'build_board' value not found in the [fujinet] section of {ini_file}.")
        exit(1)

# Boot images each family of boards needs in the flash filesystem, by build_board prefix.
# Unset packs no images (every image together does not fit the 4 MiB storage partition),
# so a board that has an obvious answer gets it here; others pick their own (see distfiles/esp32-data/boot/README.md).
DEFAULT_BOOT_IMAGES = {
    "atari-": "atari",
}

def default_boot_images(board_name):
    for prefix, images in DEFAULT_BOOT_IMAGES.items():
        if board_name.startswith(prefix):
            return images
    return None

def create_local_ini(board_name, local_file_name):
    config = configparser.RawConfigParser()
    config.add_section('fujinet')
    config.set('fujinet', 'build_board', board_name)
    config.set('fujinet', 'console_type', "noconsole")
    boot_images = default_boot_images(board_name)
    if boot_images:
        config.set('fujinet', 'boot_images', boot_images)
    with open(local_file_name, 'w') as configfile:
        config.write(configfile)
        configfile.write("; console_type can be one of [noconsole, consolecdc, consoleuart], if unset, it defaults to noconsole\n")
        configfile.write("; boot_images picks the boot images for the flash filesystem: paths under\n")
        configfile.write("; distfiles/esp32-data/boot, space or comma separated. Unset packs none (all of them\n")
        configfile.write("; do not fit the storage partition); Atari boards default to atari.\n")
        configfile.write("; boot_images = amiga/wb32\n\n")
        configfile.write("[env]\n")
        configfile.write("; Serial port for -u / -f / -m (default /dev/ttyACM0). PlatformIO reads these from\n")
        configfile.write("; [env], not [fujinet]. A stable name via a udev rule: docs/diagnostics.md.\n")
        configfile.write(";upload_port = /dev/ttyUSB0\n;monitor_port = /dev/ttyUSB0\n")
        configfile.write(";build_flags += \n")
    print(f"{local_file_name} file created with build_board = {board_name}")

def merge_ini_files(base_file, local_file, output_file):
    # Use RawConfigParser to avoid interpolation of ${...}
    base_config = configparser.RawConfigParser()
    base_config.read(base_file)

    # Read build_board value from local.ini
    build_board = read_build_board_value(local_file)

    # Construct the filename for the specific build_board.ini file
    build_board_file = f"pio-build/ini/platforms/platformio-{build_board}.ini"
    if not os.path.exists(build_board_file):
        print(f"Error: The file {build_board_file} does not exist.")
        exit(1)

    # Files to merge, in order
    files_to_merge = [base_file, build_board_file, local_file]

    # A dictionary to hold combined values for keys using the "+=" operator
    combined_values = defaultdict(dict)

    for file in files_to_merge:
        additional_config = configparser.RawConfigParser()
        additional_config.read(file)

        for section in additional_config.sections():
            if not base_config.has_section(section):
                base_config.add_section(section)

            for key, value in additional_config.items(section):
                # Check if the key uses the special "+=" syntax
                if key.endswith("+"):
                    real_key = key.rstrip("+").strip()
                    if base_config.has_option(section, real_key):
                        # Trim whitespace and newline characters before concatenating
                        existing_value = base_config.get(section, real_key).strip()
                        new_value = value.strip()
                        combined_value = existing_value + "\n" + new_value if existing_value else new_value
                        combined_values[section][real_key] = combined_value
                    else:
                        combined_values[section][real_key] = value.strip()
                else:
                    base_config.set(section, key, value.strip())

    # Apply combined values from the "+=" operator
    for section, keys in combined_values.items():
        for key, value in keys.items():
            base_config.set(section, key, value)

    # Write the final merged INI content to a file
    with open(output_file, 'w') as merged_file:
        base_config.write(merged_file)

def merge_ini_files_simple(a_file, b_file, output_file):
    base_config = configparser.RawConfigParser()
    base_config.read(a_file)

    # Files to merge, in order
    files_to_merge = [a_file, b_file]

    # A dictionary to hold combined values for keys using the "+=" operator
    combined_values = defaultdict(dict)

    for file in files_to_merge:
        additional_config = configparser.RawConfigParser()
        additional_config.read(file)

        for section in additional_config.sections():
            if not base_config.has_section(section):
                base_config.add_section(section)

            for key, value in additional_config.items(section):
                # Check if the key uses the special "+=" syntax
                if key.endswith("+"):
                    real_key = key.rstrip("+").strip()
                    if base_config.has_option(section, real_key):
                        # Trim whitespace and newline characters before concatenating
                        existing_value = base_config.get(section, real_key).strip()
                        new_value = value.strip()
                        combined_value = existing_value + "\n" + new_value if existing_value else new_value
                        combined_values[section][real_key] = combined_value
                    else:
                        combined_values[section][real_key] = value.strip()
                else:
                    base_config.set(section, key, value.strip())

    # Apply combined values from the "+=" operator
    for section, keys in combined_values.items():
        for key, value in keys.items():
            base_config.set(section, key, value)

    # Write the final merged INI content to a file
    with open(output_file, 'w') as merged_file:
        base_config.write(merged_file)

def main():
    parser = argparse.ArgumentParser(description="platformio.ini generator")
    parser.add_argument("-n", "--new", metavar="board_name", help="Create a new platformio.local.ini file with specified board name before merging.")
    parser.add_argument("-l", "--local-file", metavar="local_file", help="Use specified local_file instead of platformio.local.ini")
    parser.add_argument("-o", "--output-file", metavar="output_file", help="write to output_file instead of default platformio.ini")
    parser.add_argument("-f", "--add-files", metavar="add_files", action="append", help="include additional ini file changes, can be specified multiple times")

    args = parser.parse_args()
    local_file = "platformio.local.ini"
    output_file = "platformio.ini"

    if args.local_file:
        local_file = args.local_file

    if args.new:
        create_local_ini(args.new, local_file)

    if args.output_file:
        output_file = args.output_file

    merge_ini_files('pio-build/ini/platformio.common.ini', local_file, output_file)

    # merge in files specified
    if args.add_files:
        for f in args.add_files:
            merge_ini_files_simple(output_file, f, output_file)

    print(f"Merged INI file created as '{output_file}'.")

if __name__ == "__main__":
    main()
