// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Release iOS and Android use the mainnet BitChat BLE service UUID.
// Set to 1 only when testing against a debug/testnet iOS build.
#ifndef BITCHAT_USE_TESTNET
#define BITCHAT_USE_TESTNET 0
#endif
