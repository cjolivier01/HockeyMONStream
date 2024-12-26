import os
import shutil
import subprocess
import tempfile


def unpack_deb(deb_file, work_dir):
    """Unpacks a .deb file into a working directory."""
    subprocess.run(["dpkg-deb", "-x", deb_file, work_dir], check=True)
    control_dir = os.path.join(work_dir, "DEBIAN")
    os.makedirs(control_dir, exist_ok=True)
    subprocess.run(["dpkg-deb", "--control", deb_file, control_dir], check=True)
    return control_dir


def modify_dependencies(control_dir):
    """Modifies the dependency versions in the control file to have no versions."""
    control_file = os.path.join(control_dir, "control")
    if not os.path.exists(control_file):
        raise FileNotFoundError(f"Control file not found in {control_dir}")

    with open(control_file, "r") as file:
        lines = file.readlines()

    modified_lines = []
    for line in lines:
        if line.startswith("Depends:"):
            # Remove version information from dependencies
            dependencies = line[len("Depends:") :].strip()
            dependencies = ", ".join(
                dep.split("(")[0].strip() for dep in dependencies.split(",")
            )
            modified_lines.append(f"Depends: {dependencies}\n")
        else:
            modified_lines.append(line)

    with open(control_file, "w") as file:
        file.writelines(modified_lines)


def repack_deb(work_dir, output_deb):
    """Repack the modified working directory into a new .deb file."""
    subprocess.run(["dpkg-deb", "--build", work_dir, output_deb], check=True)


def main():
    deb_file = "graph_composer-4.1.0_x86_64.deb"  # Input .deb file
    output_deb = "undependencies.deb"  # Output .deb file

    if not os.path.exists(deb_file):
        print(f"Error: {deb_file} not found.")
        return

    with tempfile.TemporaryDirectory() as work_dir:
        control_dir = unpack_deb(deb_file, work_dir)
        modify_dependencies(control_dir)
        repack_deb(work_dir, output_deb)

    print(f"Modified .deb file created: {output_deb}")


if __name__ == "__main__":
    main()
