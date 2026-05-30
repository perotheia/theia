// User handler bodies for FlexRayIngress.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/FlexRayIngress.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/FlexRayIngress.hh"

#include <cstdio>

namespace ara::odd_path_monitor {



void FlexRayIngress::handle_cast(const EML_01& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to EML_01 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EML_01\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const Licht_hinten_01& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to Licht_hinten_01 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received Licht_hinten_01\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_SensorHeader& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_SensorHeader (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_SensorHeader\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_ObjektHeader& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_ObjektHeader (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_ObjektHeader\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_01& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_01 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_01\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_02& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_02 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_02\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_03& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_03 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_03\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_04& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_04 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_04\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_05& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_05 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_05\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_06& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_06 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_06\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_07& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_07 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_07\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_08& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_08 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_08\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_09& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_09 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_09\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Objekt_10& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Objekt_10 (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Objekt_10\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Linien_Ego_Links& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Linien_Ego_Links (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Linien_Ego_Links\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Linien_Ego_Rechts& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Linien_Ego_Rechts (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Linien_Ego_Rechts\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Linien_Nebenspuren& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Linien_Nebenspuren (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Linien_Nebenspuren\n",
                 kNodeName);
}

void FlexRayIngress::handle_cast(const BV2_Linien_Ego_Doppel& /*msg*/,
                                 FlexRayIngressState& /*s*/) {
    // TODO: react to BV2_Linien_Ego_Doppel (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received BV2_Linien_Ego_Doppel\n",
                 kNodeName);
}




}  // namespace ara::odd_path_monitor
