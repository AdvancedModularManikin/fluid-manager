
#include <algorithm>
#include <thread>
#include <chrono>
#include <boost/algorithm/string.hpp>

#include "amm_std.h"

#include "AMM/BaseLogger.h"

#include "tinyxml2.h"

#include "spi_remote.h"


using namespace std;
using namespace std::chrono;
using namespace std::literals::string_literals;
using namespace tinyxml2;
using namespace AMM;


// Daemonize by default
int daemonize = 1;

const string actionPrefix = "ACT=";
std::string genericTopicPrefix = "[";
const string keepAlivePrefix = "[KEEPALIVE]";
const string loadScenarioPrefix = "LOAD_SCENARIO:";
const string haltingString = "HALTING_ERROR";
const string sysPrefix = "[SYS]";
const string actPrefix = "[ACT]";
const string loadPrefix = "LOAD_STATE:";

float operating_pressure = 15.0;
float purge_pressure = 15.0;
bool have_pressure = false;
bool send_status = false;
AMM::StatusValue current_status = AMM::StatusValue::INOPERATIVE;
bool module_stopped = false;
bool module_purging = false;
bool blood_reservoir_empty = false;
bool clear_reservoir_empty = false;

void ProcessConfig(const std::string &configContent) {
   tinyxml2::XMLDocument doc;
   doc.Parse(configContent.c_str());

   tinyxml2::XMLHandle docHandle(&doc);
   tinyxml2::XMLElement *entry = docHandle.FirstChildElement("Configuration").ToElement();
   tinyxml2::XMLElement *entry4 = entry->FirstChildElement("Capability")->ToElement();

   //scan for capability type=fluidics
   while (entry4) {
      if (!strcmp(entry4->ToElement()->Attribute("type"), "fluidics")) break;

      auto v = entry4->ToElement()->NextSibling();
      if (v) {
         entry4 = v->ToElement();
      } else break;
   }
   if (!entry4) {
      LOG_ERROR << "cfg data didn't contain <Capability type=fluidics>";
      return;
   }

   tinyxml2::XMLElement *enableEl = entry4->FirstChildElement("enable");
   if (enableEl != nullptr) {
      std::string enableFlag = enableEl->GetText();
      if (enableFlag == "1" || enableFlag == "true") {

      } else {
         LOG_WARNING << "Fluidics has been disabled via configuration, not setting operating pressure.";
         return;
      }
   }


   tinyxml2::XMLElement *pressureEl = entry4->FirstChildElement("operating_pressure");

   if (pressureEl != nullptr) {
      operating_pressure = stof(pressureEl->GetText());
      have_pressure = true;
      LOG_INFO << "Setting operating pressure to " << operating_pressure;
      return;
   } else {
      LOG_ERROR << "cfg data didn't contain <data name=operating_pressure>";
   }
}


class FluidListener {
public:
    void onNewCommand(AMM::Command &c, eprosima::fastrtps::SampleInfo_t *info) {
       // We received configuration which we need to push via SPI
       if (!c.message().compare(0, sysPrefix.size(), sysPrefix)) {
          ostringstream static_filename;
          std::string value = c.message().substr(sysPrefix.size());

          if (value == "START_FLUIDICS") {
             LOG_DEBUG << "Received Start Fluidics command";
             static_filename << "config/fluid_manager_configuration.xml";
             std::ifstream ifs(static_filename.str());
             std::string configContent((std::istreambuf_iterator<char>(ifs)),
                                       (std::istreambuf_iterator<char>()));

             ifs.close();

             ProcessConfig(configContent);

             current_status = AMM::StatusValue::OPERATIONAL;
          } else if (value == "STOP_FLUIDICS") {
             LOG_DEBUG << "Received Stop Fluidics command";
             module_stopped = true;
          } else if ( value == "START_PURGE") {
             LOG_DEBUG << "Received Start Purge command";
             module_purging = true;
          } else if ( value == "STOP_PURGE") {
             LOG_DEBUG << "Received Stop Purge command";
             module_purging = false;
          }
       }
    }
};

const std::string moduleName = "AMM_FluidManager";
const std::string configFile = "config/fluid_manager_amm.xml";
AMM::DDSManager<FluidListener> *mgr = new AMM::DDSManager<FluidListener>(configFile);
AMM::UUID m_uuid;

static void show_usage(const std::string &name) {
   cerr << "Usage: " << name << "\nOptions:\n"
        << "\t-h,--help\t\tShow this help message\n" << endl;
}


#define RATE_LIMIT_MOD 2<<6
int rate_limit_count = 1;
bool rate_limiter(int modulus) {
   rate_limit_count++;
   return (rate_limit_count % modulus == 0);
}

//code from air_tank.cpp initially copied here
struct pid_ctl {
    float p;
    float i;
    float d;
    float target;

    float isum; // current value
    float last;
    float last_diff;
};

float
pi_supply(struct pid_ctl *p, float reading)
{
   float diff = reading - p->last;
   p->last = reading;
   p->last_diff = diff;
   float oset = p->target - reading;

   p->isum += oset * p->i;

   return p->isum + p->p * oset + p->d * diff;
}

struct pid_ctl pid;

uint32_t stall_val = 0x100;
//PSI (atmospheric is 0)
//float operating_pressure = 5.0;

int gpio_J4 = 7 + 0;
int gpio_J5 = 7 + 1;
int gpio_J6 = 7 + 2;
int gpio_J7 = 7 + 3;
int gpio_J8 = 7 + 4;
int gpio_J9 = 7 + 5;
int gpio_J10 = 7 + 6;
int gpio_J11 = 7 + 7;

bool should_pid_run = true;
float ret;
uint32_t val;

void air_reservoir_control_task(void)
{
   LOG_INFO << "Thread for air reservoir control task";

   int solenoid_0 = 7, motor_dac = 1;
   int solenoid_A = gpio_J4;
   int solenoid_B = gpio_J5;
   int solenoid_C = gpio_J6;
   int solenoid_AC = gpio_J10;
   int solenoid_AD = solenoid_0 + 7;
   remote_set_gpio(solenoid_B, 0); // solenoid B off on startup
   remote_set_gpio(solenoid_A, 0); //solenoid A TODO to purge lines A off B on
   remote_set_gpio(solenoid_C, 0);
   int motor_enable = 16;//B1
   remote_set_gpio(motor_enable, 0); // motor disabled on startup
   //in order to purge: Turn B off, Turn A on, Turn AC & AD on
   //P1 pressure will slowly drop to atmospheric
   //p4 pressure should stay above 1 bar until the lines are clear of liquid
   //p1, p2 & p3 should remain close to each other until the reservoirs are empty
   //when purging control loop should work off of Pressure4, but pressure1 otherwise
#if 0
   //temp. purge code. leave control loop where it is, need air to purge
    remote_set_gpio(solenoid_AC, 1);
    //remote_set_gpio(solenoid_AD, 1);
    remote_set_gpio(solenoid_B, 0);
    remote_set_gpio(solenoid_A, 1);
#endif
   //adcs
   int P1 = 0, P2 = 1, P3 = 2, P4 = 3;

   pid.p = 48;   // adjusting for max speed of 1500 was p = 24;
   pid.i = 0;    // too much windup, let's do this without i = 1.0 / 1024;
   pid.d = 0;    // no need for differential here ..  was d = 1.0 / 16;
   pid.isum = 0;

   uint16_t dacVal;
   int rail_24V = 15;
   remote_set_dac(motor_dac, 0);
   remote_set_gpio(rail_24V, 1); //should_24v_be_on = 1;
   bool should_motor_run = 1;

    state_startup:
   {
      LOG_INFO << "Awaiting configuration";
      while (!have_pressure) {
         std::this_thread::sleep_for(std::chrono::milliseconds(100));
         if ( module_purging ) goto state_purge;
      }
      remote_set_gpio(rail_24V, 1);
      remote_set_gpio(motor_enable, 1);
      remote_set_gpio(solenoid_B, 1);
      remote_set_gpio(solenoid_A, 0);
      remote_set_gpio(solenoid_C, 0);
      int not_pressurized = 1;
      LOG_INFO << "Pressurizing";

      //pressurize, when done goto enter_state_operational;
      //TODO need to determine if pressure is really completed.
      while (not_pressurized) {
         if (module_stopped) goto state_error;
         if ( module_purging ) {
            LOG_DEBUG << "Please STOP FLUIDICS and connect flushing adapter before starting purge";
            module_purging = false;
         }

         pid.target = operating_pressure;
         float hold_isum = pid.isum;
         uint32_t adcRead = remote_get_adc(P1);
         float psi = ((float) adcRead) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         ret = pi_supply(&pid, psi);

         //convert back to 0-2^12 range for DAC
         val = (uint32_t)(ret * 1000.0);
         should_motor_run = stall_val < val;
         if (!should_motor_run) {
            pid.isum = hold_isum;
         }
         dacVal = val > 0xfff ? 0xfff : val;
         remote_set_dac(motor_dac, dacVal);

         float psiP4 = ((float) remote_get_adc(P4)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         if (rate_limiter(RATE_LIMIT_MOD)) {
            LOG_DEBUG << "P1: " << psi;
            LOG_DEBUG << "P4: " << psiP4;
         }

         // check if air pressure has reached the operating pressure
         if (psi > (operating_pressure - 0.25)) {
            LOG_INFO << "Pressurization complete";

            // check fluid supply pressures
            float psiP2 = ((float)remote_get_adc(P2))*(3.0/10280.0*16.0) - 15.0/8.0;
            if ( psiP2 > (operating_pressure - 1.0) ) {
               blood_reservoir_empty = false;
               AMM::Status a;
               a.module_id(m_uuid);
               a.module_name(moduleName);
               a.capability("blood_supply");
               a.value(AMM::StatusValue::OPERATIONAL);
               mgr->WriteStatus(a);
            } else {
               blood_reservoir_empty = true;
               LOG_INFO << "P2 low; blood reservoir empty";
               AMM::Status a;
               a.module_name(moduleName);
               a.module_id(m_uuid);
               a.capability("blood_supply");
               a.message("Blood supply empty");
               a.value(AMM::StatusValue::INOPERATIVE);
               mgr->WriteStatus(a);
            }

            float psiP3 = ((float)remote_get_adc(P3))*(3.0/10280.0*16.0) - 15.0/8.0;
            if ( psiP3 > (operating_pressure - 1.0) ) {
               clear_reservoir_empty = false;
               AMM::Status a;
               a.module_id(m_uuid);
               a.module_name(moduleName);
               a.capability("clear_supply");
               a.value(AMM::StatusValue::OPERATIONAL);
               mgr->WriteStatus(a);
            } else {
               clear_reservoir_empty = true;
               AMM::Status a;
               a.module_id(m_uuid);
               a.module_name(moduleName);
               a.capability("clear_supply");
               a.message("Clear supply empty");
               a.value(AMM::StatusValue::INOPERATIVE);
               mgr->WriteStatus(a);
               LOG_INFO << "P3 low; clear reservoir empty";
            }

            current_status = AMM::StatusValue::OPERATIONAL;
            send_status = true;
            goto state_operational;
         }
      }
   }

    state_operational:
   {
      LOG_INFO << "System Operational";
      remote_set_gpio(rail_24V, 1);
      remote_set_gpio(motor_enable, 1);
      remote_set_gpio(solenoid_B, 1);
      remote_set_gpio(solenoid_A, 0);
      remote_set_gpio(solenoid_C, 0);

      int stay_operational = 1; //TODO change in response to DDS commands
      while (stay_operational) {
         if (module_stopped) goto state_error;
         if ( module_purging ) {
            LOG_DEBUG << "Please STOP FLUIDICS and connect flushing adapter before starting purge";
            module_purging = false;
         }
         pid.target = operating_pressure;
         float hold_isum = pid.isum;
         uint32_t adcRead = remote_get_adc(P1);
         float psi = ((float) adcRead) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         float psiP2 = ((float)remote_get_adc(P2))*(3.0/10280.0*16.0) - 15.0/8.0;
         float psiP3 = ((float)remote_get_adc(P3))*(3.0/10280.0*16.0) - 15.0/8.0;
         float psiP4 = ((float) remote_get_adc(P4)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         if (rate_limiter(RATE_LIMIT_MOD)) {
            LOG_DEBUG << "air supply (P1): " << psi;
            //	LOG_DEBUG << "P2: " << psiP2;
            //	LOG_DEBUG << "P3: " << psiP3;
            //  LOG_DEBUG << "P4: " << psiP4;
         }

         if ( psiP2 < (operating_pressure - 1.0) && !blood_reservoir_empty ) {
            blood_reservoir_empty = true;
            LOG_INFO << "P2 low; blood reservoir empty";
         }

         if ( psiP3 < (operating_pressure - 1.0) && !clear_reservoir_empty ) {
            clear_reservoir_empty = true;
            LOG_INFO << "P3 low; clear reservoir empty";
         }

         // determine if compressore needs to be run or not
         ret = pi_supply(&pid, psi);
         //convert back to 0-2^12 range for DAC
         val = (uint32_t)(ret * 1000.0);
         should_motor_run = stall_val < val;
         if (!should_motor_run) {
            pid.isum = hold_isum;
         }
         dacVal = val > 0xfff ? 0xfff : val;
         remote_set_dac(motor_dac, dacVal);
         //TODO this thread waits on other threads in remote_ calls so it does not actually need this delay here
         std::this_thread::sleep_for(std::chrono::milliseconds(1));

         //TODO no predicate for leaving this, but leave in response to a message.
         //TODO also leave after 20s for testing purposes
         //goto state_purge;
         //printf("pressurizing psi to %f\n", psi);
      }
   }

    state_purge:
   {
      remote_set_gpio(solenoid_A, 0);
      remote_set_gpio(solenoid_B, 0);		// turn off air enable valve to let reservoir pressur ebleed off
      remote_set_gpio(motor_enable, 0);	// turn off compressor
      remote_set_dac(motor_dac, 0x0);

      bool purge_not_complete = 1;
      while (purge_not_complete) {
         if (module_stopped) goto state_error;
         if ( !module_purging ) goto state_error;

         // wait for pressure P1 (air reservoir) to bleed off
         float psiP1 = ((float) remote_get_adc(P1)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         float psiP2 = ((float) remote_get_adc(P2)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         float psiP3 = ((float) remote_get_adc(P3)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;
         float psiP4 = ((float) remote_get_adc(P4)) * (3.0 / 10280.0 * 16.0) - 15.0 / 8.0;

         if ( psiP1 < 0.1 ) {
            remote_set_gpio(solenoid_A, 1);		// turn on valve A to allow air into the fluid channels
            remote_set_gpio(motor_enable, 1);	// enable compressor
            remote_set_dac(motor_dac, 0xfff);	// turn compressor on full speed
         }

         // once the pressure P1 has dropped below 0.1 psi and purging is running, it should not come up again
         // if it does, turn the compressor off
         if ( psiP1 > 0.5 ) {
            remote_set_dac(motor_dac, 0x0);       // turn compressor speed to 0
         }

         if (rate_limiter(RATE_LIMIT_MOD)) {
            LOG_DEBUG << "P1: " << psiP1;
            LOG_DEBUG << "P2: " << psiP2;
            LOG_DEBUG << "P3: " << psiP3;
            LOG_DEBUG << "P4: " << psiP4;
         }

      }
   }
   goto state_error;

    state_error:
   {// this is also the stopped state
      //turn off motor, close all solenoids, turn off 24V rail
      LOG_INFO << "Disabling motor, resetting valves";
      remote_set_gpio(motor_enable, 0);
      remote_set_gpio(motor_dac, 0x0);
      remote_set_gpio(rail_24V, 0);
      remote_set_gpio(solenoid_B, 0);
      remote_set_gpio(solenoid_A, 0);
      remote_set_gpio(solenoid_C, 0);
      current_status = AMM::StatusValue::INOPERATIVE;
      send_status = true;

      have_pressure = false;
      module_stopped = false;
      module_purging = false;
      goto state_startup;
   }
}

void PublishOperationalDescription() {
   AMM::OperationalDescription od;
   od.name(moduleName);
   od.model("Fluid Manager");
   od.manufacturer("Entropic");
   od.serial_number("1.0.0");
   od.module_id(m_uuid);
   od.module_version("1.0.0");
   const std::string capabilities = AMM::Utility::read_file_to_string("config/fluid_manager_capabilities.xml");
   od.capabilities_schema(capabilities);
   od.description();
   mgr->WriteOperationalDescription(od);
}

void PublishConfiguration() {
   AMM::ModuleConfiguration mc;
   auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
   mc.timestamp(ms);
   mc.module_id(m_uuid);
   mc.name(moduleName);
   const std::string configuration = AMM::Utility::read_file_to_string("config/fluid_manager_configuration.xml");
   mc.capabilities_configuration(configuration);
   mgr->WriteModuleConfiguration(mc);
}


int main(int argc, char *argv[]) {
   static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
   plog::init(plog::verbose, &consoleAppender);

   host_remote_init(&remote);
   std::thread remote_thread;//(remote_task);
   std::thread air_tank_thread;//(air_reservoir_control_task);

   LOG_INFO << "=== [FluidManager] Starting up (not yet ready)";
   for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if ((arg == "-h") || (arg == "--help")) {
         show_usage(argv[0]);
         return 0;
      }

      if (arg == "-d") {
         daemonize = 1;
      }
   }

   FluidListener fl;

   mgr->InitializeCommand();
   mgr->InitializeSimulationControl();

   mgr->InitializeOperationalDescription();
   mgr->InitializeModuleConfiguration();
   mgr->InitializeStatus();

   mgr->CreateOperationalDescriptionPublisher();
   mgr->CreateModuleConfigurationPublisher();
   mgr->CreateStatusPublisher();

   mgr->CreateCommandSubscriber(&fl, &FluidListener::onNewCommand);

   m_uuid.id(mgr->GenerateUuidString());

   std::this_thread::sleep_for(std::chrono::milliseconds(250));

   PublishOperationalDescription();
   PublishConfiguration();

   remote_thread = std::thread{remote_task};
   air_tank_thread = std::thread{air_reservoir_control_task};

   bool closed = 0;

   LOG_INFO << "=== [FluidManager] Entering runtime loop.";

   while (!closed) {
      if (send_status) {
         LOG_INFO << "[FluidManager] Setting status to " << current_status;
         send_status = false;
         AMM::Status a;
         a.module_id(m_uuid);
         a.module_name(moduleName);
         a.value(current_status);
         mgr->WriteStatus(a);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
   }

   remote_thread.join();
   air_tank_thread.join();

   LOG_INFO << "=== [FluidManager] Shutdown.";

   return 0;
}
