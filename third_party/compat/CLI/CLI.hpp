#pragma once

#include <string>

namespace CLI {

class App {
 public:
  App(std::string description = {}, std::string name = {})
      : description_(std::move(description)), name_(std::move(name)) {}

  void set_version_flag(std::string flag, std::string version) {
    version_flag_ = std::move(flag);
    version_ = std::move(version);
  }

 private:
  std::string description_;
  std::string name_;
  std::string version_flag_;
  std::string version_;
};

}  // namespace CLI
