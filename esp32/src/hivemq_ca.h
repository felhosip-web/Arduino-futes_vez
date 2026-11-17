#pragma once

#include <pgmspace.h>

// ============================================================================
// HiveMQ Cloud TLS 8883 CA tanúsítvány (Let's Encrypt ISRG Root X1)
// ============================================================================
// TODO: Másold be ide a HiveMQ Cloud ISRG Root X1 CA tanúsítványt a HiveMQ Cloud Console -> Cluster -> Details oldalról.
const char HIVEMQ_CA_CERT[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "TODO_REPLACE_WITH_HIVEMQ_CLOUD_ISRG_ROOT_X1_CA_CERT\n"
    "-----END CERTIFICATE-----\n";
