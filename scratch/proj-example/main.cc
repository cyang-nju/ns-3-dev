#include "ns3/core-module.h"
#include "foo.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ScratchSimulator");

int
main(int argc, char* argv[])
{
    NS_LOG_UNCOND("Scratch Simulator");

    foo();
    
    Simulator::Run();
    Simulator::Destroy();

    return 0;
}
