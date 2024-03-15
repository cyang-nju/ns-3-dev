# Usage of custom classes

## SwitchNode
This class implements per-flow ECMP and shortcuts ns3 routing.

Besides, this class provides virtual methods for subclasses to process packet. For example:
```cpp
class P4Switch: public SwitchNode {
protected:
    void ReceiveIpv4Packet(Ptr<NetDevice> inDev, ParsedPkt parsedPkt) override {
        Ptr<NetDevice> outDev = GetEgressDev(parsedPkt);
        ProcessPacket(parsedPkt, inDev, outDev);
        SendIpv4Packet(outDev, std::move(parsedPkt));
    }

private:
    void ProcessPacket(ParsedPkt &pkt, Ptr<NetDevice> in, Ptr<NetDevice> out) {
        ...
    }
}
```

<br>

## TcpSocketCustomHelper
This class is used to set different TcpSocketBase attributes for different application.

```cpp
    auto createSocketCallback = [](Ptr<Socket> socket) {
        socket->SetCloseCallbacks (
            MakeCallback(FlowFinished),
            MakeNullCallback<void, Ptr<Socket>>()
        );
    };

    TcpSocketCustomHelper bbrSockFactHelper{"BbrSocketFactory"};
    bbrSockFactHelper.SetCreateSocketCallback(createSocketCallback);
    bbrSockFactHelper.SetAttr("InitialCwnd", UintegerValue{40});
    bbrSockFactHelper.SetCCA(TypeId::LookupByName("ns3::TcpBbr"));
    bbrSockFactHelper.InstallAll();

    TcpSocketCustomHelper cubicSockFactHelper{"CubicSocketFactory"};
    cubicSockFactHelper.SetCreateSocketCallback(createSocketCallback);
    cubicSockFactHelper.SetCCA(TypeId::LookupByName("ns3::TcpCubic"));
    cubicSockFactHelper.InstallAll();

    BulkSendHelper bbrSendAppHelper{"BbrSocketFactory", Address{}};
    BulkSendHelper cubicSendAppHelper{"CubicSocketFactory", Address{}};
```

<br>

## misc

```cpp
namespace ns3 {
// "Range-based for-loop" for NodeContainer and NetDeviceContainer
auto begin(NodeContainer& nodes) { return nodes.Begin(); }
auto end(NodeContainer& nodes) { return nodes.End(); }
auto begin(NetDeviceContainer& devs) { return devs.Begin(); }
auto end(NetDeviceContainer& devs) { return devs.End(); }

auto begin(const NodeContainer& nodes) { return nodes.Begin(); }
auto end(const NodeContainer& nodes) { return nodes.End(); }
auto begin(const NetDeviceContainer& devs) { return devs.Begin(); }
auto end(const NetDeviceContainer& devs) { return devs.End(); }
}

void setup() {
    NodeContainer nodes;
    ...
    for (Ptr<Node> node : nodes) {
        ...
    }
    ...
}



// print simulation progress (simulated seconds)
void PrintProgress() {
    std::clog << "\e[2K\e[0G" // erase current line
              << Simulator::Now().GetSeconds() << " seconds Simulated.  ";
}

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

int main() {
    ...
    StartPeriodicCallback<PrintProgress>(MilliSeconds(20));
    Simulator::Run();
    ...
}
```


<br>

## JsonConfig

**Note:**
- JsonConfig depends on macro `CURR_SRC_DIR_ABSOLUTE` which is defined in [./CMakeLists.txt](./CMakeLists.txt) and `nlohmann-json.hpp` which is fetched from github in [./CMakeLists.txt](./CMakeLists.txt)
- json array is not supported. If you need to use array, you can save it as string in json file and manually parse it.


**Initialization:**
```cpp
    // print the filename of used config files and the content of config (using std::clog)
    JsonConfig::EnableLog();

    // ns3::Config::SetDefault is called for values under path "ConfigDefault".
    JsonConfig::Init({"test.json"});
```

<br>

**Print the content of config to file:**
```cpp
    std::ofstream outputFile{"<filePath>"};
    JsonConfig::Print(outputFile);
```

<br>

**Exmaples:**
```cpp
    // reading values
    int n = (int)JsonConfig::GetUInt("n");
    std::string foobar = JsonConfig::GetString({"foo", "bar"}); // foo.bar

    // read string as ns3::StringValue and pass it to SeAttribute
    p2p.SetDeviceAttribute("DataRate", JsonConfig::GetStringValue("LinkRate"));
    p2p.SetChannelAttribute("Delay", JsonConfig::GetStringValue("LinkDelay"));

    // read string as class T via operator>>(std::istream&, T&)
    DataRate rate = JsonConfig::Get<DataRate>("LinkRate");

    // check the existence of key
    if (JsonConfig::Contains("EnableSomething")) {
        ...
    }

    // use default value if key not exist
    JsonConfig::GetBoolOrNull("EnableSomething").value_or(false);
    JsonConfig::GetOrNull<DataRate>("LinkRate").value_or(DataRate{"1Gbps"});

    // you can also do something like this
    if (auto k = JsonConfig::GetUIntOrNull("k"); t.has_value()) {
        for (int i = 0; i < (*k); i++) {
            ...
        }
    } else {
        ...
    }
```