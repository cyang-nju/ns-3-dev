#pragma once
#include "ns3/tcp-socket-factory.h"
#include "ns3/node-container.h"
#include "ns3/net-device-container.h"
#include <functional>
#include <memory>

using namespace ns3;

/// This class is used to set different TcpSocketBase attributes for different node set.
class TcpSocketCustomFactory: public ns3::TcpSocketFactory {
public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override { return m_tid; }
    Ptr<Socket> CreateSocket() override;

private:
    using AttrPair = std::pair<Ptr<const AttributeAccessor>, Ptr<AttributeValue>>;
    using AttrList = std::vector<AttrPair>;

    struct Data {
        TypeId ccaTypeId;
        AttrList socketAttributes;
        std::function<void(Ptr<Socket>)> createSocketCallback;
    };

    TypeId m_tid;
    std::shared_ptr<Data> m_data;
    
public:
    TcpSocketCustomFactory(TypeId tid, std::shared_ptr<Data> data)
        : m_tid{tid}, m_data{data} {}

    friend class TcpSocketCustomHelper;
};



class TcpSocketCustomHelper {
    std::shared_ptr<TcpSocketCustomFactory::Data> m_data;
    TypeId m_sockFactTid;

public:
    TcpSocketCustomHelper(const std::string &name);

    void SetCCA(TypeId ccaTypeId);
    void SetAttr(const std::string &name, const AttributeValue &value);
    void SetCreateSocketCallback(std::function<void(Ptr<Socket>)> callback);

    void Install(Ptr<Node> node);
    void Install(NodeContainer c);
    void InstallAll();

    void CopySettingsFrom(const TcpSocketCustomHelper &other);
};
