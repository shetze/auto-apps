#include <vsomeip/vsomeip.hpp>
#include <sstream>
#include "controller-protocol.hpp"

// Services accept a bounded runtime workload GET/SET method from controller.
#define DEMO_TUNE_METHOD_ID 0x0008
// Root controller exposes a read-only FIELD and a gated SET method.
#define CONTROLLER_SERVICE_ID 0x1004
#define CONTROLLER_INSTANCE_ID 1
#define CONTROLLER_SET_METHOD_ID 0x0001
#define CONTROLLER_EVENTGROUP_ID 0x4010
#define CONTROLLER_STATUS_EVENT_ID 0x8010
// QM-local systemd agent, never allowed to modify host/root units.
#define CONTROLLER_AGENT_SERVICE_ID 0x1005
#define CONTROLLER_AGENT_INSTANCE_ID 1
#define CONTROLLER_AGENT_APPLY_METHOD_ID 0x0001

// Fixed SOME/IP client identifier for the ncurses multimedia client.
#define MULTIMEDIA_CLIENT_ID 0x1002
#define RADIO_SERVICE_ID 0x1000
#define RADIO_INSTANCE_ID 1

#define RADIO_IS_PLAYING_METHOD_ID 1
#define RADIO_SET_PLAYING_METHOD_ID 2
#define RADIO_GET_VOLUME_METHOD_ID 3
#define RADIO_CHANGE_VOLUME_METHOD_ID 4
#define RADIO_SWITCH_STATION_METHOD_ID 5
// Client reports actual moving-image presentation (1 Hz lease).
#define RADIO_SET_VIDEO_ACTIVE_METHOD_ID 6

#define RADIO_EVENTGROUP_ID 0x4001

// Demo-only station event value indicating RAM allocation failure.
#define RADIO_RAM_FAILURE_MARKER "__RADIO_DEMO_RAM_FAILURE__"
#define RADIO_VOLUME_EVENT_ID 0x8000 + 1
#define RADIO_STATION_EVENT_ID 0x8000 + 2
#define RADIO_SONG_EVENT_ID 0x8000 + 3
#define RADIO_ARTIST_EVENT_ID 0x8000 + 4
// Demo-only health telemetry is separate from the radio metadata eventgroup.
#define RADIO_HEALTH_EVENTGROUP_ID 0x4004
#define RADIO_HEALTH_EVENT_ID 0x8000 + 5

#define ENGINE_SERVICE_ID 0x1001
#define ENGINE_INSTANCE_ID 1

#define ENGINE_GET_REVERSE_METHOD_ID 1
// Idempotent set: payload 0=OFF, 1=ON, reply echoes actual state.
#define ENGINE_SET_PARKING_HOLD_METHOD_ID 2

#define ENGINE_EVENTGROUP_ID 0x4002
#define ENGINE_REVERSE_EVENT_ID 0x8000 + 1
// Demo-only health event uses a separate eventgroup, so multimedia-service does
// not receive diagnostic traffic when it subscribes to reverse events.
#define ENGINE_HEALTH_EVENTGROUP_ID 0x4003
#define ENGINE_HEALTH_EVENT_ID 0x8000 + 2
// Authoritative Drive/Reverse/Fast/Parking field, with a 1 Hz heartbeat.
// The legacy reverse boolean above stays available to sensors-service.
#define ENGINE_MODE_EVENTGROUP_ID 0x4008
#define ENGINE_MODE_EVENT_ID 0x8000 + 3
// Current Parking hold setting (FIELD, same eventgroup as Engine mode).
#define ENGINE_PARKING_HOLD_EVENT_ID 0x8000 + 4

// Simulated rear camera and ultrasonic sensor service. Camera frames are sent
// over a reliable (TCP) SOME/IP event; the other two independent eventgroups
// allow clients to subscribe to only the information they require.
#define SENSORS_SERVICE_ID 0x1003
#define SENSORS_INSTANCE_ID 1
#define SENSORS_CAMERA_EVENTGROUP_ID 0x4005
#define SENSORS_ULTRASONIC_EVENTGROUP_ID 0x4006
#define SENSORS_CAMERA_STATUS_EVENTGROUP_ID 0x4007
#define SENSORS_CAMERA_FRAME_EVENT_ID 0x8001
#define SENSORS_ULTRASONIC_EVENT_ID 0x8002
#define SENSORS_CAMERA_STATUS_EVENT_ID 0x8003
#define SENSORS_HEALTH_EVENTGROUP_ID 0x4009
#define SENSORS_HEALTH_EVENT_ID 0x8004

static uint32_t inline
payload_get_arg(std::shared_ptr<vsomeip::payload> its_payload)
{
  vsomeip::byte_t *data = its_payload->get_data();
  vsomeip::length_t l = its_payload->get_length();

  if (l < 4)
    return 0;

  return (uint32_t)data[0]<< 24 | data[1]<< 16 | data[2]<< 8 | data[3];
}

static std::string inline
payload_get_string(std::shared_ptr<vsomeip::payload> its_payload)
{
  vsomeip::byte_t *data = its_payload->get_data();
  vsomeip::length_t l = its_payload->get_length();
  std::stringstream ss;

  for (vsomeip::length_t i=0; i<l; i++)
    ss << (unsigned char)data[i];

  return ss.str();
}


static std::shared_ptr<vsomeip::payload> inline
payload_with_arg(uint32_t arg)
{
  std::shared_ptr<vsomeip::payload> its_payload = vsomeip::runtime::get()->create_payload();
  std::vector<vsomeip::byte_t> its_payload_data;
  its_payload_data.push_back((arg >> 24) & 0xff);
  its_payload_data.push_back((arg >> 16) & 0xff);
  its_payload_data.push_back((arg >> 8) & 0xff);
  its_payload_data.push_back((arg >> 0) & 0xff);
  its_payload->set_data(its_payload_data);

  return its_payload;
}

static std::shared_ptr<vsomeip::payload> inline
payload_with_string(std::string arg)
{
  std::shared_ptr<vsomeip::payload> its_payload = vsomeip::runtime::get()->create_payload();
  std::vector<vsomeip::byte_t> its_payload_data;
  for (vsomeip::byte_t i=0; i<arg.size(); i++) {
      its_payload_data.push_back(arg[i]);
  }
  its_payload->set_data(its_payload_data);

  return its_payload;
}

static std::shared_ptr< vsomeip::message > inline
new_request (uint32_t method, uint32_t arg)
{
  std::shared_ptr< vsomeip::message > request;
  request = vsomeip::runtime::get()->create_request();
  request->set_service(RADIO_SERVICE_ID);
  request->set_instance(RADIO_INSTANCE_ID);
  request->set_method(method);

  std::shared_ptr<vsomeip::payload> its_payload = payload_with_arg (arg);
  request->set_payload(its_payload);

  return request;
}

static uint32_t inline
get_arg0(const std::shared_ptr<vsomeip::message> _request) {
    std::shared_ptr<vsomeip::payload> its_payload = _request->get_payload();

    return payload_get_arg(its_payload);
}

static std::string inline
get_arg0_string(const std::shared_ptr<vsomeip::message> _request) {
    std::shared_ptr<vsomeip::payload> its_payload = _request->get_payload();

    return payload_get_string(its_payload);
}

static std::shared_ptr<vsomeip::message> inline
response (const std::shared_ptr<vsomeip::message> &_request,
          uint32_t arg)
{
    std::shared_ptr<vsomeip::message> its_response = vsomeip::runtime::get()->create_response(_request);
    std::shared_ptr<vsomeip::payload> its_payload = payload_with_arg (arg);

    its_response->set_payload(its_payload);

    return its_response;
}
