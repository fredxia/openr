/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/config/Config.h>

namespace {
openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string nodeName = "",
    const std::string domainName = "domain",
    const std::vector<openr::thrift::AreaConfig>& areaCfg = {},
    bool enableV4 = true,
    bool enableSegmentRouting = false,
    bool orderedFibProgramming = false,
    bool dryrun = true) {
  openr::thrift::LinkMonitorConfig linkMonitorConfig;
  *linkMonitorConfig.include_interface_regexes_ref() =
      std::vector<std::string>{"et[0-9].*"};
  *linkMonitorConfig.exclude_interface_regexes_ref() =
      std::vector<std::string>{"eth0"};
  *linkMonitorConfig.redistribute_interface_regexes_ref() =
      std::vector<std::string>{"lo1"};

  openr::thrift::KvstoreConfig kvstoreConfig;

  openr::thrift::SparkConfig sparkConfig;
  *sparkConfig.hello_time_s_ref() = 2;
  *sparkConfig.keepalive_time_s_ref() = 1;
  *sparkConfig.fastinit_hello_time_ms_ref() = 50;
  *sparkConfig.hold_time_s_ref() = 2;
  *sparkConfig.graceful_restart_time_s_ref() = 6;

  openr::thrift::OpenrConfig config;

  *config.node_name_ref() = nodeName;
  *config.domain_ref() = domainName;
  config.enable_v4_ref() = enableV4;
  config.enable_segment_routing_ref() = enableSegmentRouting;
  config.enable_ordered_fib_programming_ref() = orderedFibProgramming;
  config.dryrun_ref() = dryrun;

  *config.kvstore_config_ref() = kvstoreConfig;
  *config.link_monitor_config_ref() = linkMonitorConfig;
  *config.spark_config_ref() = sparkConfig;

  *config.enable_rib_policy_ref() = true;

  if (areaCfg.empty()) {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = "0";
    *areaConfig.neighbor_regexes_ref() = {".*"};
    *areaConfig.interface_regexes_ref() = {".*"};
    config.areas_ref()->emplace_back(areaConfig);
  } else {
    for (const auto& areaCfg : areaCfg) {
      config.areas_ref()->emplace_back(areaCfg);
    }
  }

  return config;
}

// utility function to construct thrift::AreaConfig
openr::thrift::AreaConfig
createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes) {
  openr::thrift::AreaConfig areaConfig;
  *areaConfig.area_id_ref() = areaId;
  *areaConfig.neighbor_regexes_ref() = neighborRegexes;
  *areaConfig.interface_regexes_ref() = interfaceRegexes;
  return areaConfig;
}
} // namespace
