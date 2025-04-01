#include <SimpleBLE.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// GoPro BLE UUIDs
const std::string GOPRO_BASE_UUID = "0000fea6-0000-1000-8000-00805f9b34fb";
const std::string GOPRO_COMMAND_CHARACTERISTIC = "b5f90072-aa8d-11e3-9046-0002a5d5c51b";
const std::string GOPRO_RESPONSE_CHARACTERISTIC = "b5f90073-aa8d-11e3-9046-0002a5d5c51b";
const std::string GOPRO_WIFI_SSID_CHARACTERISTIC = "b5f90074-aa8d-11e3-9046-0002a5d5c51b";
const std::string GOPRO_WIFI_PASSWORD_CHARACTERISTIC = "b5f90075-aa8d-11e3-9046-0002a5d5c51b";

// GoPro Commands (from Open GoPro API)
const std::vector<uint8_t> CMD_POWER_ON = {0x01, 0x01};
const std::vector<uint8_t> CMD_POWER_OFF = {0x01, 0x00};
const std::vector<uint8_t> CMD_START_RECORD = {0x03, 0x01, 0x01, 0x01};
const std::vector<uint8_t> CMD_STOP_RECORD = {0x03, 0x01, 0x01, 0x00};

class GoProBLEController {
 private:
  SimpleBLE::Peripheral camera;
  SimpleBLE::Service goProService;
  bool isConnected = false;

  // Helper to send commands to the GoPro
  bool sendCommand(const std::vector<uint8_t>& command) {
    try {
      if (!isConnected) {
        std::cerr << "Not connected to a GoPro camera" << std::endl;
        return false;
      }

      // Find the correct UUID for the characteristic
      for (auto& service : camera.services()) {
        if (service.uuid() == GOPRO_BASE_UUID) {
          for (auto& characteristic : service.characteristics()) {
            if (characteristic.uuid() == GOPRO_COMMAND_CHARACTERISTIC) {
              // Write to the characteristic using the peripheral object
              if (characteristic.can_write_request()) {
                camera.write_request(service.uuid(), characteristic.uuid(), command);
                std::cout << "Command sent successfully" << std::endl;
                return true;
              } else if (characteristic.can_write_command()) {
                camera.write_command(service.uuid(), characteristic.uuid(), command);
                std::cout << "Command sent successfully" << std::endl;
                return true;
              } else {
                std::cerr << "Characteristic doesn't support writing" << std::endl;
                return false;
              }
            }
          }
        }
      }

      std::cerr << "Command characteristic not found" << std::endl;
      return false;
    } catch (const std::exception& e) {
      std::cerr << "Error sending command: " << e.what() << std::endl;
      return false;
    }
  }

 public:
  GoProBLEController() {
    // Initialize the BLE adapter
    SimpleBLE::Adapter adapter = (SimpleBLE::Adapter::get_adapters())[0];
    adapter.scan_start();
    std::cout << "Scanning for GoPro cameras..." << std::endl;
  }

  ~GoProBLEController() {
    if (isConnected) {
      disconnect();
    }
  }

  // Find and connect to a GoPro camera
  bool connect() {
    try {
      SimpleBLE::Adapter adapter = (SimpleBLE::Adapter::get_adapters())[0];

      // Scan for 5 seconds
      adapter.scan_for(5000);
      auto peripherals = adapter.scan_get_results();

      // Look for a GoPro device
      for (auto& peripheral : peripherals) {
        std::cout << "Found device: " << peripheral.identifier() << std::endl;
        if (peripheral.identifier().find("GoPro") != std::string::npos) {
          std::cout << "Found GoPro camera: " << peripheral.identifier() << std::endl;
          camera = peripheral;

          // Connect to the camera
          camera.connect();
          std::cout << "Connected to GoPro camera" << std::endl;

          // Get the GoPro service
          for (auto& service : camera.services()) {
            if (service.uuid() == GOPRO_BASE_UUID) {
              goProService = service;
              break;
            }
          }

          isConnected = true;
          return true;
        }
      }

      std::cerr << "No GoPro camera found" << std::endl;
      return false;
    } catch (const std::exception& e) {
      std::cerr << "Error connecting to GoPro: " << e.what() << std::endl;
      return false;
    }
  }

  // Disconnect from the camera
  void disconnect() {
    if (isConnected) {
      camera.disconnect();
      isConnected = false;
      std::cout << "Disconnected from GoPro camera" << std::endl;
    }
  }

  // Power on the camera
  bool powerOn() {
    std::cout << "Powering on GoPro camera..." << std::endl;
    return sendCommand(CMD_POWER_ON);
  }

  // Power off the camera
  bool powerOff() {
    std::cout << "Powering off GoPro camera..." << std::endl;
    return sendCommand(CMD_POWER_OFF);
  }

  // Start recording
  bool startRecording() {
    std::cout << "Starting recording..." << std::endl;
    return sendCommand(CMD_START_RECORD);
  }

  // Stop recording
  bool stopRecording() {
    std::cout << "Stopping recording..." << std::endl;
    return sendCommand(CMD_STOP_RECORD);
  }
};

int main() {
  std::cout << "GoPro BLE Controller" << std::endl;

  GoProBLEController controller;

  // Connect to the camera
  if (!controller.connect()) {
    std::cerr << "Failed to connect to GoPro camera" << std::endl;
    return 1;
  }

  // Power on the camera
  if (!controller.powerOn()) {
    std::cerr << "Failed to power on the camera" << std::endl;
    controller.disconnect();
    return 1;
  }

  // Wait for camera to fully power on
  std::cout << "Waiting for camera to initialize..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(3));

  // Start recording
  if (!controller.startRecording()) {
    std::cerr << "Failed to start recording" << std::endl;
    controller.powerOff();
    controller.disconnect();
    return 1;
  }

  // Record for one minute
  std::cout << "Recording for 60 seconds..." << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(60));

  // Stop recording
  if (!controller.stopRecording()) {
    std::cerr << "Failed to stop recording" << std::endl;
    controller.powerOff();
    controller.disconnect();
    return 1;
  }

  // Wait for the recording to finish processing
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Power off the camera
  if (!controller.powerOff()) {
    std::cerr << "Failed to power off the camera" << std::endl;
    controller.disconnect();
    return 1;
  }

  // Disconnect from the camera
  controller.disconnect();

  std::cout << "GoPro recording session completed successfully!" << std::endl;
  return 0;
}
