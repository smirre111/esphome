#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"
#include <sstream>
#include <queue>




namespace esphome
{
namespace rikastove
{

#define RIKASTOVE_READ_BUFFER_LENGTH 255

enum State {
  STATE_IDLE = 0,
  STATE_SEND_SMS
};




class RikaStove : public climate::Climate, public uart::UARTDevice, public Component
{
public:
  void setup() override;

  
  //If a polling component
  //void update() override;
  //If a normal component
  void update();
  void loop() override;
  void dump_config() override;

  void set_supports_cool(bool supports_cool) { this->supports_cool_ = supports_cool; }
  void set_supports_heat(bool supports_heat) { this->supports_heat_ = supports_heat; }
  void set_phone_number(std::string phone_number) { this->phone_number_ = phone_number; }
  void set_pin_code(std::string pin_code) { this->pin_code_ = pin_code; }

protected:
  /// Override control to change settings of the climate device.
  void control(const climate::ClimateCall &call) override;
  /// Return the traits of this controller.
  climate::ClimateTraits traits() override;

  /// Transmit via IR the state of this climate controller.
  void transmit_state_(bool mode_change, bool temp_change);

  void parse_buffer_(const std::string &);
  void parse_reply_(const std::string &);

  void send_retour_();
  void send_ok_();
  void send_error_();

  State state_{STATE_IDLE};

  //std::string status_message_{""};
  std::queue<std::string> command_queue_;

  char read_buffer_[RIKASTOVE_READ_BUFFER_LENGTH];
  size_t read_pos_{0};
  uint8_t parse_index_{0};
  bool initialized_{false};
  bool query_status_{false};

  bool supports_cool_{true};
  bool supports_heat_{true};
  std::string phone_number_ = "+436508012415";
  std::string pin_code_ = "1211";
  const std::string date_ = {"70/01/01"};
  const std::string time_ = {"01:00:00"};

  const char ASCII_CR  = 0x0D;
  const char ASCII_LF  = 0x0A;
  const char ASCII_ESC = 0x1A;

  char term_char_ = {ASCII_CR}; 

};

} // namespace rikastove
} // namespace esphome
