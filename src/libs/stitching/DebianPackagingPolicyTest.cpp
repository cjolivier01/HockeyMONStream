#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
std::string read(const char* path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::string& text, const std::string& token) {
  return text.find(token) != std::string::npos;
}

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 13) {
    std::cerr << "FAIL: expected desktop/Jetson/Windows build and release policy inputs\n";
    return 1;
  }
  const std::string bazelrc = read(argv[1]);
  const std::string dockerfile = read(argv[2]);
  const std::string builder = read(argv[3]);
  const std::string packager = read(argv[4]);
  const std::string docker_runner = read(argv[5]);
  const std::string installer = read(argv[6]);
  const std::string jetson_builder = read(argv[7]);
  const std::string hugin_builder = read(argv[8]);
  const std::string publisher = read(argv[9]);
  const std::string windows_builder = read(argv[10]);
  const std::string windows_nsis = read(argv[11]);
  const std::string windows_powershell = read(argv[12]);
  bool ok = true;
  ok &= expect(
      contains(bazelrc, "build:deb_ubuntu24 --repo_env=CUDA_PATH=/usr/local/cuda-13.2") &&
          !contains(bazelrc, "build:deb_ubuntu24 --repo_env=CUDA_PATH=/usr/local/cuda-12"),
      "Ubuntu 24 package config must use the same CUDA 13.2 ABI as DeepStream 9.1");
  ok &= expect(
      contains(dockerfile, "cuda-compiler-13-2") && !contains(dockerfile, "cuda-compiler-12") &&
          contains(dockerfile, "\"libnvinfer-headers-dev=${trt_version}\"") &&
          contains(dockerfile, "\"libnvinfer10=${trt_version}\"") && !contains(dockerfile, "Pin-Priority"),
      "package builder must use CUDA 13.2 and transaction-local TensorRT pins without a broad APT pin");
  ok &= expect(
      contains(builder, "TARGET_CUDA_ROOT=/usr/local/cuda-13.2") &&
          !contains(builder, "TARGET_CUDA_ROOT=/usr/local/cuda-12"),
      "every target-OS build must select CUDA 13.2");
  ok &= expect(
      contains(packager, "X-HStream-Target-Ubuntu: ${TARGET_UBUNTU}") &&
          contains(packager, "X-HStream-Target-Platform: ${TARGET_PLATFORM}") &&
          contains(packager, "EXPECTED_CUDA_SONAME") && contains(packager, "unexpected CUDA major") &&
          contains(packager, "pretrained/native-calibration") && contains(packager, "model_cache_root"),
      "package must carry platform provenance, enforce each platform's CUDA ABI, and stage verified native models");
  ok &= expect(
      contains(docker_runner, ":/root/.cache/hstream/models:ro") && contains(docker_runner, "HSTREAM_MODEL_CACHE_DIR"),
      "immutable Docker build must expose the content-addressed native model cache read-only");
  ok &= expect(
      contains(installer, "X-HStream-Target-Ubuntu") && !contains(installer, "libc6 (>= 2.43)") &&
          !contains(installer, "Pin-Priority") && contains(installer, "old_deepstream_packages") &&
          contains(installer, "^deepstream-[0-9]+([.][0-9]+)*$") &&
          contains(installer, "deepstream-9.1-transition.deb") && contains(installer, "Conflicts") &&
          contains(installer, "Replaces") && !contains(installer, "apt-get remove -y --no-install-recommends") &&
          !contains(installer, "nccl") && !contains(installer, "NCCL"),
      "installer must validate OS provenance, replace older DeepStream atomically, and leave NCCL policy untouched");
  ok &= expect(
      contains(installer, "hstream-cuda-ubuntu2404-compat.gpg") && contains(installer, "disable_cuda_compat_sources") &&
          contains(installer, "disable_installer_managed_cuda_sources") &&
          contains(installer, "CUDA_LEGACY_COMPAT_SOURCE") && contains(installer, "publish_cuda_compat_source") &&
          contains(installer, "restore_compat_source_transition") && contains(installer, "sync -d") &&
          contains(installer, "sync -f") && contains(installer, "Enabled: no") &&
          !contains(installer, "rollback_transaction") &&
          !contains(installer, "install -m 0644 \"${combined_keyring}\" /usr/share/keyrings/cuda-archive-keyring.gpg"),
      "Ubuntu 26 installer must own its key and interruption-safely replace duplicate compatibility sources");
  ok &= expect(
      contains(bazelrc, "build:deb_jetson --@rules_cuda//cuda:archs=sm_87") &&
          contains(jetson_builder, "--config=opt --config=deb_jetson") &&
          contains(jetson_builder, "output_base=\"${persistent_cache_root}/output\"") &&
          !contains(jetson_builder, "--disk_cache") && contains(jetson_builder, "bazelisk --batch") &&
          contains(jetson_builder, "--sandbox_base=\"${sandbox_base}\"") &&
          contains(jetson_builder, "--action_env=TMPDIR=/var/tmp") &&
          contains(jetson_builder, "--sandbox_tmpfs_path=/var/tmp") && contains(jetson_builder, "--list-elf") &&
          contains(jetson_builder, "--list-ptx") && contains(jetson_builder, "apt-get -s install"),
      "Jetson release packages must contain verified native Orin code and pass an install simulation");
  ok &= expect(
      contains(packager, "nvidia-l4t-cuda | libcuda.so.1") &&
          contains(packager, "libopencv (>= %s), libopencv (<< %s)") &&
          contains(packager, "jetson_opencv_upper_version") && contains(packager, "HSTREAM_HUGIN_TOOLS_DIR") &&
          contains(hugin_builder, "HUGIN_SHA256=") && contains(hugin_builder, "VIGRA_SHA256=") &&
          contains(hugin_builder, "--export-sources") && contains(hugin_builder, "autooptimiser") &&
          contains(hugin_builder, "nona"),
      "Jetson packages must use L4T/OpenCV dependencies and pinned source-built Hugin calibration tools");
  ok &= expect(
      contains(publisher, "X-HStream-Source-Commit") && contains(publisher, "--repo \"${repository}\"") &&
          contains(publisher, "git remote get-url --push --all origin") &&
          contains(publisher, "hugin_2022.0.0+dfsg.orig.tar.xz") && contains(publisher, "10#${patch}") &&
          contains(publisher, "(0|[1-9][0-9]*)") && contains(publisher, "windows-wsl-setup.exe") &&
          contains(publisher, "sha256sum ./*.deb ./*.exe") && contains(publisher, "\"${release_dir}\"/*.exe") &&
          contains(publisher, "WINDOWS_INSTALLER_REPOSITORY=\"${repository}\"") &&
          contains(publisher, "WINDOWS_SIGNING_PKCS12") && contains(publisher, "WINDOWS_SIGNING_CA_FILE") &&
          contains(publisher, "-CAfile") && contains(publisher, "private/self-signed publisher certificate") &&
          contains(publisher, "osslsigncode verify"),
      "release publication must verify provenance/source compliance, increment strict semver, and publish Windows setup");
  ok &= expect(
      contains(windows_builder, "makensis") && contains(windows_builder, "rsvg-convert") &&
          contains(windows_builder, "icotool") && contains(windows_builder, "PE32 executable") &&
          contains(windows_builder, "^v(0|[1-9][0-9]*)") && contains(windows_builder, "sha256sum") &&
          contains(windows_builder, "POWERSHELL_SHA256") && contains(windows_builder, "osslsigncode sign") &&
          contains(windows_builder, "-readpass") && contains(windows_builder, "WINDOWS_SIGNING_CA_FILE") &&
          contains(windows_builder, "-CAfile") && contains(windows_builder, "osslsigncode verify"),
      "Windows setup must cross-build a versioned native executable and preserve the HStream icon");
  ok &= expect(
      contains(windows_nsis, "File /oname=hstream-wsl.ps1") &&
          contains(windows_nsis, "File /oname=install-hstream-deb") &&
          !contains(windows_nsis, "File /oname=hstream.deb") && !contains(windows_nsis, "File /oname=deepstream.deb") &&
          contains(windows_nsis, "RequestExecutionLevel user") &&
          !contains(windows_nsis, "RequestExecutionLevel admin") &&
          contains(windows_nsis, "Select NVIDIA DeepStream") && contains(windows_nsis, "MB_DEFBUTTON2") &&
          contains(windows_nsis, "permanently deletes") && contains(windows_nsis, "NSD_CreatePassword") &&
          contains(windows_nsis, "SetEnvironmentVariableW") && !contains(windows_nsis, "-GitHubToken") &&
          contains(windows_nsis, "ExecShellWait \"runas\"") && contains(windows_nsis, "POWERSHELL_SHA256") &&
          contains(windows_nsis, "ReadAllBytes") && contains(windows_nsis, "SHA256") &&
          contains(windows_nsis, "ScriptBlock]::Create") && contains(windows_nsis, "EnsureWslMachine") &&
          contains(windows_nsis, "HStream was not removed") && contains(windows_nsis, "Abort"),
      "Windows setup must remain a small bootstrapper, keep credentials off command lines, and confirm data deletion");
  ok &= expect(
      contains(windows_powershell, "Download-VerifiedFile") && contains(windows_powershell, "Get-FileHash") &&
          contains(windows_powershell, "cloud-images.ubuntu.com/wsl/releases/24.04/20240423") &&
          contains(windows_powershell, "UbuntuRootfsSha256") &&
          contains(windows_powershell, "releases/download/$VersionTag") &&
          contains(windows_powershell, "api.github.com/repos/$RepositoryName/releases/assets") &&
          contains(windows_powershell, "HSTREAM_GITHUB_TOKEN") &&
          contains(windows_powershell, "wsl.2.7.11.0.x64.msi") &&
          contains(windows_powershell, "A611DDACEE689D2FB1FB5319E58AF7F3998864D86CDCE632EADD8E61614A0F9D") &&
          contains(windows_powershell, "MinimumWslVersion") && contains(windows_powershell, "Get-WslRuntimeVersion") &&
          contains(windows_powershell, "WslPrerequisiteExitCode") && contains(windows_powershell, "EnsureWslMachine") &&
          !contains(windows_powershell, "-EncodedCommand") && !contains(windows_powershell, "-Verb RunAs") &&
          contains(windows_powershell, "[Environment]::SystemDirectory") &&
          contains(windows_powershell, "$env:PSModulePath = $WindowsPowerShellModules") &&
          contains(windows_powershell, "Dism\\Get-WindowsOptionalFeature") &&
          !contains(windows_powershell, "FilePath \"wsl.exe\"") && contains(windows_powershell, "HStream-WSL-") &&
          contains(windows_powershell, "[Guid]::NewGuid()") && contains(windows_powershell, "icacls.exe") &&
          contains(windows_powershell, "/inheritance:r") && contains(windows_powershell, "*S-1-5-18:(OI)(CI)F") &&
          contains(windows_powershell, "*S-1-5-32-544:(OI)(CI)F") &&
          contains(windows_powershell, "Get-AuthenticodeSignature") &&
          contains(windows_powershell, "O=Microsoft Corporation") && contains(windows_powershell, "msiexec.exe") &&
          contains(windows_powershell, "\"/qn\"") && !contains(windows_powershell, "wsl.exe --install") &&
          contains(windows_powershell, "^ID=ubuntu$") && contains(windows_powershell, "^VERSION_ID=.*24[.]04.*$") &&
          contains(windows_powershell, "/lib64/ld-linux-x86-64.so.2") &&
          contains(windows_powershell, "PROCESSOR_ARCHITEW6432") &&
          contains(windows_powershell, "Test-HStreamDistroOwnership") &&
          contains(windows_powershell, "hstream-wsl-bootstrapper-schema-1") &&
          contains(windows_powershell, "Refusing to unregister") &&
          contains(windows_powershell, "pending-wsl-import.json") &&
          contains(windows_powershell, "wsl-installation.json") &&
          contains(windows_powershell, "Write-WslRegistrationRecord") &&
          contains(windows_powershell, "Test-WslRegistrationRecord") &&
          contains(windows_powershell, "RegistrationId = $RegistrationId") &&
          contains(windows_powershell, "$record.Schema -eq 2") &&
          contains(windows_powershell, "Get-WslDistroRegistration") &&
          contains(windows_powershell, "Remove-AbandonedWslImport") &&
          contains(windows_powershell, "Bind-PendingWslRegistration") &&
          contains(windows_powershell, "^WSL-[0-9a-f]{32}$") &&
          contains(windows_powershell, "Removing the incomplete HStream WSL import") &&
          contains(windows_powershell, "--import\", $DistroName") &&
          contains(windows_powershell, "install ok installed $expectedHStreamVersion") &&
          contains(windows_powershell, "install ok installed 9.1.0-1+resolute2") &&
          contains(windows_powershell, "/usr/lib/wsl/lib/libcuda.so.1") &&
          contains(windows_powershell, "Package: hstream-wsl-libcuda") &&
          !contains(windows_powershell, "nvidia-driver") && !contains(windows_powershell, "cuda-drivers"),
      "WSL provisioning must verify downloads, isolate HStream, and use only the Windows-projected CUDA driver");
  return ok ? 0 : 1;
}
