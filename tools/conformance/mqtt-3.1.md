## MQTT 3.1

MQTT 3.1 (IBM and Eurotech, 2010) numbers no statements. What follows is
where it differs from 3.1.1 in what a Server must do. Elsewhere 3.1 is taken
to be as 3.1.1.

| Where 3.1 differs | Test |
|---|---|
| Protocol name `MQIsdp`, level 3 | `mqtt_packet_test.cpp:MqttPacket.MQTT31_ConnectSpeaksMQIsdpLevel3` |
| CONNACK's first byte is reserved: no Session Present flag | `mqtt_packet_test.cpp:MqttPacket.MQTT31_ConnackHasNoSessionPresentFlag` |
| SUBACK carries a granted QoS per filter and no failure code | `mqtt_packet_test.cpp:MqttPacket.MQTT31_SubackHasNoFailureCode` |
| A client identifier of 1 to 23 characters; outside that, CONNACK return code 2 | not yet (the connection's) |
| WebSocket subprotocol `mqttv3.1` | not yet (the connection's) |
