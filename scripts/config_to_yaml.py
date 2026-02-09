#!/usr/bin/python3
import sys
import configparser
import yaml

def convert_ini_to_yaml(ini_file, yaml_file):
    # Read the .ini file
    config = configparser.ConfigParser()
    config.read(ini_file)

    # Convert to a dictionary
    config_dict = {section: dict(config.items(section)) for section in config.sections()}

    # Write the dictionary to a YAML file
    with open(yaml_file, 'w') as file:
        yaml.dump(config_dict, file, default_flow_style=False)

def main():
    if len(sys.argv) != 2:
        print("Usage: python convert_ini_to_yaml.py <config_file.ini>")
        sys.exit(1)

    ini_file = sys.argv[1]
    yaml_file = ini_file.rsplit('.', 1)[0] + '.yaml'

    try:
        convert_ini_to_yaml(ini_file, yaml_file)
        print(f"Converted {ini_file} to {yaml_file}")
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()

