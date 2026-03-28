#include "pch.h"
#include "NvApiTypes.h"

#include <fakenvapi_inc.h>
#include <include/nvapi/nvapi_interface.h>

NvApiTypes::NvApiTypes()
{
    for (const auto& entry : nvapi_interface_table)
    {
        lookupTable[entry.func] = entry.id;
    }
    for (const auto& entry : fakenvapi_interface_table)
    {
        lookupTable[entry.func] = entry.id;
    }
}
NvApiTypes& NvApiTypes::Instance()
{
    static NvApiTypes instance;
    return instance;
}

unsigned int NvApiTypes::getId(const std::string& name) const
{
    auto it = lookupTable.find(name);
    if (it != lookupTable.end())
    {
        return it->second;
    }
    LOG_TRACE("Not a known nvapi interface");
    return 0;
}
