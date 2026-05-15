#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

void mqttInit();
void mqttLoop();
void mqttApplyConfig(const String& json);
String mqttGetStatusJson();
void mqttFillStatusJson(JsonObject& out, bool includePreview = true);
bool mqttIsConnected();
void mqttForcePublish();
