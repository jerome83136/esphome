#pragma once

#if defined(USE_ESP32)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <esp_now.h>
#include <esp_crc.h>

#include <array>
#include <memory>
#include <queue>
#include <vector>
#include <mutex>
#include <map>

namespace esphome {
namespace espnow {

typedef uint8_t espnow_addr_t[6];

static const uint64_t ESPNOW_BROADCAST_ADDR = 0xFFFFFFFFFFFF;
static espnow_addr_t ESPNOW_ADDR_SELF = {0};
static const uint8_t MAX_ESPNOW_DATA_SIZE = 240;

static const uint32_t TRANSPORT_HEADER = 0xC19983;

template<typename... Args> std::string string_format(const std::string &format, Args... args) {
  int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1;  // Extra space for '\0'
  if (size_s <= 0) {
    return ("Error during formatting.");
  }
  auto size = static_cast<size_t>(size_s);
  std::unique_ptr<char[]> buf(new char[size]);
  std::snprintf(buf.get(), size, format.c_str(), args...);
  return std::string(buf.get(), buf.get() + size - 1);  // We don't want the '\0' inside
}

static uint8_t last_ref_id = 0;

struct ESPNowPacket {
  uint64_t mac64 = 0;
  uint8_t size = 0;
  uint8_t rssi = 0;
  uint8_t retrys : 4;
  uint8_t is_broadcast : 1;
  uint8_t dummy : 3;
  uint32_t timestamp = 0;

  union {
    uint8_t content[MAX_ESPNOW_DATA_SIZE + 11];
    struct {
      uint8_t header[3] = {0xC1, 0x99, 0x83};
      uint32_t app_id = 0xFFFFFF;
      uint8_t ref_id = 0x99;
      uint16_t crc16 = 0x1234;
      uint8_t data[MAX_ESPNOW_DATA_SIZE];
      uint8_t space = 0;
    };
  };

  inline ESPNowPacket() ESPHOME_ALWAYS_INLINE : retrys(0) {}
  inline ESPNowPacket(uint64_t mac64, const uint8_t *data, uint8_t size, uint32_t app_id);

  inline void info(std::string place);

  inline void get_mac(espnow_addr_t *mac_addres) { std::memcpy(mac_addres, &mac64, 6); }
  inline void set_mac(espnow_addr_t *mac_addres) { this->mac64 = this->to_mac64(mac_addres); }

  uint64_t to_mac64(espnow_addr_t *mac_addres) {
    uint64_t result;
    std::memcpy(&result, mac_addres, 6);
    return result;
  }

  void retry() {
    if (this->retrys < 7) {
      retrys = retrys + 1;
    }
  }

  inline void recalc() {
    crc16 = 0;
    crc16 = esp_crc16_le(ref_id, (uint8_t *) &content, 10 + size);
  }

  bool is_valid();

  inline std::string to_str(uint64_t mac64 = 0) {
    espnow_addr_t mac;
    if (mac64 == 0)
      mac64 = this->mac64;
    memcpy((void *) &mac, &mac64, 6);
    return string_format("{\"%02x:%02x:%02x:%02x:%02x:%02x\"}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  inline uint8_t *dataptr() { return (uint8_t *) &content; }
};

class ESPNowComponent;

static const uint32_t ESPNOW_DEFAULT_APP_ID = 0x11CFAF;

class ESPNowProtocol : public Parented<ESPNowComponent> {
 public:
  ESPNowProtocol(){};

  void setup();

  virtual void on_receive(ESPNowPacket packet){};
  virtual void on_sent(ESPNowPacket packet, bool status){};
  virtual void on_new_peer(ESPNowPacket packet){};

  virtual uint32_t get_app_id() = 0;
  uint8_t get_next_ref_id() { return next_ref_id_++; }

  bool write(uint64_t mac_address, const uint8_t *data, uint8_t len);
  bool write(uint64_t mac_address, std::vector<uint8_t> &data);
  bool write(ESPNowPacket packet);

 protected:
  uint8_t next_ref_id_{0};
};

class ESPNowDefaultProtocol : public ESPNowProtocol {
 public:
  void on_receive(ESPNowPacket packet) override { this->on_receive_.call(packet); };
  void on_sent(ESPNowPacket packet, bool status) override { this->on_sent_.call(packet, status); };
  void on_new_peer(ESPNowPacket packet) override { this->on_new_peer_.call(packet); };

  uint32_t get_app_id() override { return ESPNOW_DEFAULT_APP_ID; };

  void add_on_sent_callback(std::function<void(ESPNowPacket, bool status)> &&callback) {
    this->on_sent_.add(std::move(callback));
  }
  void add_on_receive_callback(std::function<void(ESPNowPacket)> &&callback) {
    this->on_receive_.add(std::move(callback));
  }
  void add_on_peer_callback(std::function<void(ESPNowPacket)> &&callback) {
    this->on_new_peer_.add(std::move(callback));
  }

 protected:
  CallbackManager<void(ESPNowPacket, bool)> on_sent_;
  CallbackManager<void(ESPNowPacket)> on_receive_;
  CallbackManager<void(ESPNowPacket)> on_new_peer_;
};

class ESPNowComponent : public Component {
 public:
  ESPNowComponent();

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 1)
  static void on_data_received(const esp_now_recv_info_t *recv_info, const uint8_t *data, int size);
#else
  static void on_data_received(const uint8_t *addr, const uint8_t *data, int size);
#endif

  static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status);

  void dump_config() override;

  float get_setup_priority() const override { return -100; }

  void set_wifi_channel(uint8_t channel) { this->wifi_channel_ = channel; }
  void set_auto_add_peer(bool value) { this->auto_add_peer_ = value; }
  void set_use_sent_check(bool value) { this->use_sent_check_ = value; }

  void setup() override;

  void runner();
  void loop() override;

  bool write(ESPNowPacket packet);

  void register_protocol(ESPNowProtocol *protocol) {
    protocol->set_parent(this);
    this->protocols_[protocol->get_app_id()] = protocol;
  }

  esp_err_t add_peer(uint64_t addr);
  esp_err_t del_peer(uint64_t addr);

  bool send_queue_empty() { return uxQueueMessagesWaiting(this->send_queue_) == 0; }
  bool send_queue_full() { return uxQueueSpacesAvailable(this->send_queue_) == 0; }
  size_t send_queue_used() { return uxQueueMessagesWaiting(this->send_queue_); }
  size_t send_queue_free() { return uxQueueSpacesAvailable(this->send_queue_); }

  void lock() { this->lock_ = true; }
  bool is_locked() { return this->lock_; }
  void unlock() { this->lock_ = false; }

  ESPNowDefaultProtocol *get_default_protocol();

 protected:
  bool validate_channel_(uint8_t channel);

  uint8_t wifi_channel_{0};

  bool auto_add_peer_{false};
  bool use_sent_check_{true};
  bool lock_{false};

  void on_receive_(ESPNowPacket packet);
  void on_sent_(ESPNowPacket packet, bool status);
  void on_new_peer_(ESPNowPacket packet);

  QueueHandle_t receive_queue_{};
  QueueHandle_t send_queue_{};

  std::map<uint32_t, ESPNowProtocol *> protocols_{};
  std::vector<uint64_t> peers_{};

  TaskHandle_t espnow_task_handle_{nullptr};
};

template<typename... Ts> class SendAction : public Action<Ts...>, public Parented<ESPNowComponent> {
 public:
  template<typename V> void set_mac(V mac) { this->mac_ = mac; }
  void set_data_template(std::function<std::vector<uint8_t>(Ts...)> func) {
    this->data_func_ = func;
    this->static_ = false;
  }
  void set_data_static(const std::vector<uint8_t> &data) {
    this->data_static_ = data;
    this->static_ = true;
  }

  void play(Ts... x) override {
    auto mac = this->mac_.value(x...);

    if (this->static_) {
      this->parent_->get_default_protocol()->write(mac, this->data_static_);
    } else {
      auto val = this->data_func_(x...);
      this->parent_->get_default_protocol()->write(mac, val);
    }
  }

 protected:
  TemplatableValue<uint64_t, Ts...> mac_{};
  bool static_{false};
  std::function<std::vector<uint8_t>(Ts...)> data_func_{};
  std::vector<uint8_t> data_static_{};
};

template<typename... Ts> class NewPeerAction : public Action<Ts...>, public Parented<ESPNowComponent> {
 public:
  template<typename V> void set_mac(V mac) { this->mac_ = mac; }
  void play(Ts... x) override {
    auto mac = this->mac_.value(x...);
    parent_->add_peer(mac);
  }

 protected:
  TemplatableValue<uint64_t, Ts...> mac_{};
};

template<typename... Ts> class DelPeerAction : public Action<Ts...>, public Parented<ESPNowComponent> {
 public:
  template<typename V> void set_mac(V mac) { this->mac_ = mac; }
  void play(Ts... x) override {
    auto mac = this->mac_.value(x...);
    parent_->del_peer(mac);
  }

 protected:
  TemplatableValue<uint64_t, Ts...> mac_{};
};

class ESPNowSentTrigger : public Trigger<ESPNowPacket, bool> {
 public:
  explicit ESPNowSentTrigger(ESPNowComponent *parent) {
    parent->get_default_protocol()->add_on_sent_callback(
        [this](ESPNowPacket value, bool status) { this->trigger(value, status); });
  }
};

class ESPNowReceiveTrigger : public Trigger<ESPNowPacket> {
 public:
  explicit ESPNowReceiveTrigger(ESPNowComponent *parent) {
    parent->get_default_protocol()->add_on_receive_callback([this](ESPNowPacket value) { this->trigger(value); });
  }
};

class ESPNowNewPeerTrigger : public Trigger<ESPNowPacket> {
 public:
  explicit ESPNowNewPeerTrigger(ESPNowComponent *parent) {
    parent->get_default_protocol()->add_on_peer_callback([this](ESPNowPacket value) { this->trigger(value); });
  }
};

extern ESPNowComponent *global_esp_now;

}  // namespace espnow
}  // namespace esphome

#endif
