#include "rika_stove.h"
#include "esphome/core/log.h"

namespace esphome
{
namespace rikastove
{

static const char *TAG = "rikastove.climate";

const char ASCII_CR = 0x0D;
const char ASCII_LF = 0x0A;

const uint32_t COOLIX_OFF = 0xB27BE0;

const uint8_t COOLIX_COOL = 0b00;
const uint8_t COOLIX_DRY = 0b01;
const uint8_t COOLIX_AUTO = 0b10;
const uint8_t COOLIX_HEAT = 0b11;

const float POWER_MIN = 30.0;
const float POWER_MAX = 100.0;
const uint8_t POWER_STEP = 5;

climate::ClimateTraits RikaStove::traits()
{
  auto traits = climate::ClimateTraits();
  traits.set_supports_current_temperature(true);
  traits.set_supports_auto_mode(true);
  traits.set_supports_cool_mode(this->supports_cool_);
  traits.set_supports_heat_mode(this->supports_heat_);
  traits.set_supports_two_point_target_temperature(false);
  traits.set_supports_away(false);
  traits.set_visual_min_temperature(17);
  traits.set_visual_max_temperature(30);
  traits.set_visual_temperature_step(1);
  return traits;
}

void RikaStove::dump_config()
{
  //ESP_LOGCONFIG(TAG, "Stove '%s':", this->stove_->get_name().c_str());
}

void RikaStove::setup()
{

  this->current_temperature = 20.0f;
  // restore set points
  auto restore = this->restore_state_();
  if (restore.has_value())
  {
    restore->apply(this);
  }
  else
  {
    // restore from defaults
    this->mode = climate::CLIMATE_MODE_OFF;
    // initialize target temperature to some value so that it's not NAN
    this->target_temperature = roundf(this->current_temperature);
  }

  // The first message that we will exchange with stove is the tTEL command to be able
  // to receive messages from the stove
  this->command_queue_.push("TEL");
}

void RikaStove::control(const climate::ClimateCall &call)
{
  bool mode_change = false;
  bool temp_change = false;
  if (call.get_mode().has_value())
  {
    this->mode = *call.get_mode();
    mode_change = true;
  }

  if (call.get_target_temperature().has_value())
  {
    this->target_temperature = *call.get_target_temperature();
    temp_change = true;
  }
  //Tell the stove that there is new state
  this->transmit_state_(mode_change, temp_change);

  //Publish the state through native API
  this->publish_state();
}

void RikaStove::transmit_state_(bool mode_change, bool temp_change)
{

  std::string mode_string;
  std::string mode_short;

  ESP_LOGD(TAG, "Vmin '%f:", this->traits().get_visual_min_temperature());
  ESP_LOGD(TAG, "Vmax '%f:", this->traits().get_visual_max_temperature());

  switch (this->mode)
  {
  case climate::CLIMATE_MODE_COOL:
    mode_string = "ROOM";
    mode_short = "r";
    break;
  case climate::CLIMATE_MODE_HEAT:
    mode_string = "HEAT";
    mode_short = "h";
    break;
  case climate::CLIMATE_MODE_AUTO:
    mode_short = "AUTO";
    mode_string = "AUTO";
    break;
  case climate::CLIMATE_MODE_OFF:
  default:
    mode_short = "OFF";
    mode_string = "OFF";
    break;
  }

  uint8_t temp;
  std::string api_command;

  if (this->mode != climate::CLIMATE_MODE_OFF)
  {
    temp = (uint8_t)roundf(clamp(this->target_temperature, this->traits().get_visual_min_temperature(), this->traits().get_visual_max_temperature()));
    //In HEAT mode we have to supply a power value between 30% and 100%
    //We misuse the HEAT mode for the power mode
    if (this->mode == climate::CLIMATE_MODE_HEAT)
    {
      //Convert the temperature for the HEAT mode to a per cent value in power
      float power = (temp - this->traits().get_visual_min_temperature()) * ((POWER_MAX - POWER_MIN) / (this->traits().get_visual_max_temperature() - this->traits().get_visual_min_temperature())) + POWER_MIN;
      // The power level needs to be set in 5% steps from 30 to 100%
      temp = ((uint8_t)roundf(power / POWER_STEP)) * POWER_STEP;
    }

    //The temperature/power level needs to be converted into a string
    std::ostringstream ss;
    std::string temp_str;
    ss << (int)temp;
    temp_str = ss.str();

    //If the mode was changed, we only specify the new mode
    if (mode_change == true)
    {
      api_command = mode_string;
    }
    else
    {
      //If the target temperature was changed we specify the short form of the current mode plus the temperature
      if ((this->mode == climate::CLIMATE_MODE_HEAT) || (this->mode == climate::CLIMATE_MODE_COOL))
      {
        api_command = mode_short + temp_str;
      }
    }
  }
  else
  {
    api_command = "OFF";
  }

  ESP_LOGD(TAG, "Sending message: %s", api_command.c_str());

  std::string temp_message;
  if (api_command == "OFF")
  {
    temp_message = api_command;
  }
  else if (api_command == "AUTO")
  {
    temp_message = "TEL";
  }
  else
  {
    temp_message = "?";
  }

  this->command_queue_.push(temp_message);
}

void RikaStove::loop()
{
  // Read message
  while (this->available())
  {
    uint8_t byte;
    this->read_byte(&byte);

    if (this->read_pos_ == RIKASTOVE_READ_BUFFER_LENGTH)
      this->read_pos_ = 0;

    //ESP_LOGD(TAG, "Buffer pos: %u %d", this->read_pos_, byte); // NOLINT

    if (byte >= 0x7F)
      byte = '?'; // need to be valid utf8 string for log functions.
    this->read_buffer_[this->read_pos_] = byte;

    if (this->read_buffer_[this->read_pos_] == ASCII_CR)
    {
      ESP_LOGD(TAG, "Buffer : %s", this->read_buffer_);
      this->read_buffer_[this->read_pos_] = 0;
      this->read_pos_ = 0;
      this->parse_cmd_(std::string(this->read_buffer_));
    }
    else
    {
      this->read_pos_++;
    }
  }
}

void RikaStove::update()
{
  ESP_LOGV(TAG, "In update");
  // In case we want add periodic reading of the state / temperature using a polling component
  // this function will be called periodically by the core
}

void RikaStove::parse_cmd_(const std::string &at_cmd_buffer)
{

  ESP_LOGD(TAG, "Received at_cmd_buffer: %s", at_cmd_buffer.c_str());
  if (at_cmd_buffer.empty())
    return;

  // The stove sends an SMS
  if (at_cmd_buffer.rfind("AT+CMGS", 0) == 0)
  {
    // on donne l'invite >
    this->send_retour_();
    this->write_str(">");
    //Affichage
    ESP_LOGD(TAG, "-> Send SMS");
    ESP_LOGD(TAG, "-> Message:");

    // Get the content of the SMS
    // Allow the stove to answer
    delay(2000);
    status_message_ = "";
    byte rcv_byte = 0;
    char rcv_char = 0;
    while (rcv_char != char(26))
    {
      if (this->available())
      {
        this->read_byte(&rcv_byte);
        rcv_char = (char)rcv_byte;
        if (rcv_char != char(26))
        { // ctrl+z (ASCII 26) pour finir le SMS
          status_message_ += rcv_char;
        }
      }
    }

    //TODO: Parse the reply
    this->parse_reply_(status_message_);

    ESP_LOGD(TAG, "-> Status message: %s", status_message_.c_str());
    ESP_LOGD(TAG, "-> +CMGS : 01");

    //Send the response
    this->send_retour_();
    this->write_str("+CMGS : 01");
    this->send_retour_();
    this->send_ok_();
  }
  else if (at_cmd_buffer.rfind("AT+CMGR", 0) == 0)
  { // The stove wants to read an SMS

    ESP_LOGD(TAG, "-> Received SMS");
    if (this->command_queue_.empty() == true)
    {
      ESP_LOGD(TAG, "-> %s", "NONE");
    }

    if (this->command_queue_.empty() == false)
    //else if (current_message_ != "NONE")
    { // There is a command to send

      std::string current_command;
      current_command = this->command_queue_.front();

      std::string reply = "+CMGR: \"REC READ\",\"";
      reply += phone_number_;
      reply += "\",,\"";
      reply += date_;
      reply += ",";
      reply += time_;
      reply += "+08\"";

      // Message for the stove
      this->send_retour_();

      this->write_str(reply.c_str());
      ESP_LOGD(TAG, "-> Message: %s", reply.c_str());

      this->send_retour_();
      reply = pin_code_;
      reply += " ";
      reply += current_command;

      this->write_str(reply.c_str());
      ESP_LOGD(TAG, "-> Message: %s", reply.c_str());

      this->send_retour_();
      this->send_retour_();
      this->send_ok_();
      delay(2000);
      query_status_ = true;
    }
    else
    { // There is no comand to send to the stove
      this->send_retour_();
      this->send_ok_();
    }
  }
  else if (at_cmd_buffer.rfind("AT+CMGD", 0) == 0)
  { // The stove asks to delete the current SMS

    ESP_LOGD(TAG, "-> Deleted message: %s", command_queue_.front().c_str());
    this->command_queue_.pop();

    this->send_ok_();
  }
  else if (at_cmd_buffer.rfind("ATE0", 0) == 0)
  {
    this->send_ok_();
  }
  else if (at_cmd_buffer.rfind("AT+CNMI", 0) == 0)
  {
    this->send_ok_();
  }
  else if (at_cmd_buffer.rfind("AT+CMGF", 0) == 0)
  {
    this->send_ok_();
  }
  else if ((at_cmd_buffer != "") && (at_cmd_buffer != "\n") && (at_cmd_buffer != "\x1A") && (at_cmd_buffer != "\x0D"))
  {
    this->send_error_();
  }
}

void RikaStove::parse_reply_(const std::string &stove_reply)
{
  size_t pos;

  //STOVE ON - HEAT 30
  //STOVE ON - HEAT 100
  //STOVE ON - ROOM 22
  //STOVE OFF
  //FAILURE: code error
  //FAILURE: text error
  std::istringstream is(stove_reply);
  std::string part;
  std::vector<std::string> str_vec;
  while (getline(is, part, ' '))
  {
    str_vec.push_back(part);
  }

  std::string rply;
  std::string mode;
  std::string value;

  rply = str_vec[0];

  if (rply == "STOVE")
  {
    if (str_vec[1] == "ON")
    {
      mode = str_vec[3];
      if (str_vec.size() > 4)
      {
        value = str_vec[4];
        std::stringstream ss(value);
        int temp_int = 0;
        ss >> temp_int;
        //For HEAT mode we need to re-scale power into temperature
        if (mode == "HEAT")
        {
          float power = (temp_int - POWER_MIN) * ((this->traits().get_visual_max_temperature() - this->traits().get_visual_min_temperature()) / (POWER_MAX - POWER_MIN)) + this->traits().get_visual_min_temperature();
          temp_int = (uint8_t)roundf(power);
        }
        this->current_temperature = (float)temp_int;
        this->publish_state();
      }

      ESP_LOGV(TAG, "Stove reply: OK: %s", stove_reply.c_str());
    }
    else if (str_vec[1] == "OFF")
    {
      ESP_LOGV(TAG, "Stove reply: OK: %s", stove_reply.c_str());
    }
    else
    {
      ESP_LOGW(TAG, "Stove reply: UNKNOWN: %s", stove_reply.c_str());
    }
  }
  else if (rply == "FAILURE:")
  {
    ESP_LOGE(TAG, "Stove reply: FAILURE: %s", stove_reply.c_str());
  }
  else
  {
    ESP_LOGE(TAG, "Stove reply: UNKNOWN: %s", stove_reply.c_str());
  }
}

void RikaStove::send_retour_()
{ // on envoie CR + LF au poele
  this->write_byte(char(13));
  this->write_byte(char(10));
}
void RikaStove::send_ok_()
{ // on envoie OK au poele
  this->write_str("OK");
  ESP_LOGD(TAG, "-> Response: OK");
  this->send_retour_();
}
void RikaStove::send_error_()
{
  this->write_str("ERROR");
  ESP_LOGD(TAG, "-> Response: ERROR");
  this->send_retour_();
}

} // namespace rikastove
} // namespace esphome
