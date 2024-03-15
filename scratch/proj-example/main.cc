#include "ns3/core-module.h"
#include "ns3/net-device-container.h"
#include "ns3/point-to-point-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/applications-module.h"

#include "json-config.h"
#include "switch-node.h"
#include "tcp-socket-custom-factory.h"

using namespace ns3;


using NonArgCallback = void (*)();

template <NonArgCallback func>
void __PeriodicCallback(Time interval) {
    func();
    Simulator::Schedule(interval, __PeriodicCallback<func>, interval);
}

template <NonArgCallback func>
void StartPeriodicCallback(Time interval, Time offset = Time{0}) {
    Simulator::Schedule(interval + offset, __PeriodicCallback<func>, interval);
}

void PrintProgress() {
    std::clog << "\e[2K\e[0G" // erase current line
              << Simulator::Now().GetSeconds() << " seconds Simulated.  ";
}


namespace ns3 {
// add support for Range-based for-loop
auto begin(NodeContainer& nodes) { return nodes.Begin(); }
auto end(NodeContainer& nodes) { return nodes.End(); }
auto begin(NetDeviceContainer& devs) { return devs.Begin(); }
auto end(NetDeviceContainer& devs) { return devs.End(); }

auto begin(const NodeContainer& nodes) { return nodes.Begin(); }
auto end(const NodeContainer& nodes) { return nodes.End(); }
auto begin(const NetDeviceContainer& devs) { return devs.Begin(); }
auto end(const NetDeviceContainer& devs) { return devs.End(); }
}


int
main(int argc, char* argv[])
{
    CommandLine cmd{__FILE__};
    cmd.Parse(argc, argv);

    JsonConfig::EnableLog();
    JsonConfig::Init({"example.json"});

    return 0;
}
