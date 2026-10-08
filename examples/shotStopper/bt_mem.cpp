// arduino-esp32 3.3+ frees the BT controller's memory at boot unless a BT
// library claims it. ArduinoBLE doesn't, so BLE.begin() would initialize the
// controller in freed heap and crash (assert in tlsf block_locate_free).
// This strong definition overrides the core's weak btInUse() to keep it.
#ifdef ESP32
extern "C" bool btInUse(void) { return true; }
#endif
