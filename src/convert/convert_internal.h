// Shared state for the converter stages.
#pragma once
#include "convert/convert.h"
#include "core/dat.h"
#include "game/proto.h"

namespace fo {

struct ConvContext {
    const ConvertOptions& opt;
    DatArchive dat;
    std::unordered_map<int, Proto> protos; // filled by convertProtos (needed to decode map objects)
};

std::string convOut(const ConvContext& c, const std::string& rel);
bool convWrite(const ConvContext& c, const std::string& rel, const Bytes& data);
bool convWriteText(const ConvContext& c, const std::string& rel, const std::string& text);

bool convertProtos(ConvContext& c);  // convert_proto.cpp
bool convertMaps(ConvContext& c);    // convert_map.cpp
bool convertPremade(ConvContext& c); // convert_misc.cpp
bool convertFonts(ConvContext& c);   // convert_misc.cpp

} // namespace fo
