#include "tcp-socket-custom-factory.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/tcp-socket-base.h"

NS_OBJECT_ENSURE_REGISTERED(TcpSocketCustomFactory);

TypeId TcpSocketCustomFactory::GetTypeId() {
    static TypeId tid = TypeId{"TcpSocketCustomFactory"}.SetParent<SocketFactory>();
    return tid;
}

Ptr<Socket> TcpSocketCustomFactory::CreateSocket() {
    Ptr<Socket> socket;
    if (m_data->ccaTypeId.GetUid() == 0) {
        socket = GetObject<TcpL4Protocol>()->CreateSocket();
    } else {
        socket = GetObject<TcpL4Protocol>()->CreateSocket(m_data->ccaTypeId);
    }
    for (auto &[accessor, value] : m_data->socketAttributes) {
        accessor->Set(PeekPointer(socket), *value);
    }
    if (m_data->createSocketCallback) {
        m_data->createSocketCallback(socket);
    }
    return socket;
}



TcpSocketCustomHelper::TcpSocketCustomHelper(const std::string &name) {
    static std::map<std::string, TypeId> sockFactTidMap;
    m_data = std::make_shared<TcpSocketCustomFactory::Data>();
    if (name.empty()) {
        m_sockFactTid = TcpSocketCustomFactory::GetTypeId();
    } else {
        auto it = sockFactTidMap.find(name);
        if (it == sockFactTidMap.end()) {
            m_sockFactTid = TypeId{name};
            m_sockFactTid.SetParent<SocketFactory>();
            m_sockFactTid.SetSize(sizeof(TcpSocketCustomFactory));
            sockFactTidMap[name] = m_sockFactTid;
        } else {
            m_sockFactTid = it->second;
        }
    }
}

void TcpSocketCustomHelper::SetCCA(TypeId ccaTypeId) {
    m_data->ccaTypeId = ccaTypeId;
}

void TcpSocketCustomHelper::SetAttr(const std::string &name, const AttributeValue &value) {
    TypeId::AttributeInformation info;
    TypeId socketTid = TcpSocketBase::GetTypeId();
    if (!socketTid.LookupAttributeByName(name, &info)) {
        NS_FATAL_ERROR("Invalid attribute set (" << name << ") on " << socketTid.GetName ());
        return;
    }
    Ptr<AttributeValue> v = info.checker->CreateValidValue(value);
    if (v == nullptr) {
        NS_FATAL_ERROR ("Invalid value for attribute set (" << name << ") on " << socketTid.GetName ());
        return;
    }
    m_data->socketAttributes.emplace_back(info.accessor, v);
}

void TcpSocketCustomHelper::SetCreateSocketCallback(std::function<void(Ptr<Socket>)> callback) {
    m_data->createSocketCallback = std::move(callback);
}

void TcpSocketCustomHelper::Install(Ptr<Node> node) {
    node->AggregateObject(CreateObject<TcpSocketCustomFactory>(m_sockFactTid, m_data));
}

void TcpSocketCustomHelper::Install(NodeContainer c) {
    for (auto it = c.Begin(); it != c.End(); it++) {
        Install(*it);
    }
}

void TcpSocketCustomHelper::InstallAll() {
    Install(NodeContainer::GetGlobal());
}

void TcpSocketCustomHelper::CopySettingsFrom(const TcpSocketCustomHelper &other) {
    *m_data = *other.m_data;
}