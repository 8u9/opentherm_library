/*
OpenTherm.cpp - OpenTherm Communication Library For Arduino, ESP8266
Copyright 2018, Ihor Melnyk
*/

#include "OpenTherm.h"

OpenTherm::OpenTherm(int inPin, int outPin, bool isSlave):
	status(OpenThermStatus::NOT_INITIALIZED),
	inPin(inPin),
	outPin(outPin),
	isSlave(isSlave),
	response(0),
	responseStatus(OpenThermResponseStatus::NONE),
	responseTimestamp(0),
	handleInterruptCallback(NULL),
	processResponseCallback(NULL)
{
}

void OpenTherm::begin(void(*handleInterruptCallback)(void), void(*processResponseCallback)(unsigned long, OpenThermResponseStatus))
{
	pinMode(inPin, INPUT);
	pinMode(outPin, OUTPUT);
	if (handleInterruptCallback != NULL) {
		this->handleInterruptCallback = handleInterruptCallback;
		attachInterrupt(digitalPinToInterrupt(inPin), handleInterruptCallback, CHANGE);
	}
	activateBoiler();
	status = OpenThermStatus::READY;
	this->processResponseCallback = processResponseCallback;
}

void OpenTherm::begin(void(*handleInterruptCallback)(void))
{
	begin(handleInterruptCallback, NULL);
}

bool IRAM_ATTR OpenTherm::isReady()
{
	return status == OpenThermStatus::READY;
}

int IRAM_ATTR OpenTherm::readState() {
	return digitalRead(inPin);
}

void OpenTherm::setActiveState() {
	digitalWrite(outPin, LOW);
}

void OpenTherm::setIdleState() {
	digitalWrite(outPin, HIGH);
}

void OpenTherm::activateBoiler() {
	setIdleState();
	delay(1000);
}

void OpenTherm::sendBit(bool high) {
	if (high) setActiveState(); else setIdleState();
	delayMicroseconds(500);
	if (high) setIdleState(); else setActiveState();
	delayMicroseconds(500);
}

bool OpenTherm::sendRequestAync(unsigned long request)
{
	Serial.println("sendRequestAync MessageID: ("+String(OpenThermMessageIDToString(getDataID(request)))+") currentStatus:("+String(stautsToString(status))+") Request: (" + String(request, HEX)+") HEX");
	
	noInterrupts();
	const bool ready = isReady();

	if (!ready) {
		interrupts();
		return false;
    }

	status = OpenThermStatus::REQUEST_SENDING;
	response = 0;
	responseStatus = OpenThermResponseStatus::NONE;

	sendBit(HIGH); //start bit
	for (int i = 31; i >= 0; i--) {
		sendBit(bitRead(request, i));
	}
	sendBit(HIGH); //stop bit
	setIdleState();

	status = OpenThermStatus::RESPONSE_WAITING;
	responseTimestamp = micros();
	interrupts();
	return true;
}

unsigned long OpenTherm::sendRequest(unsigned long request)
{	
	if (!sendRequestAync(request)) return 0;
	while (!isReady()) {
		process();
		yield();
	}
	return response;
}

bool OpenTherm::sendResponse(unsigned long request)
{
	status = OpenThermStatus::REQUEST_SENDING;
	response = 0;
	responseStatus = OpenThermResponseStatus::NONE;

	sendBit(HIGH); //start bit
	for (int i = 31; i >= 0; i--) {
		sendBit(bitRead(request, i));
	}
	sendBit(HIGH); //stop bit
	setIdleState();
	status = OpenThermStatus::READY;
	return true;
}

unsigned long OpenTherm::getLastResponse()
{
	return response;
}

OpenThermResponseStatus OpenTherm::getLastResponseStatus()
{
	return responseStatus;
}

void IRAM_ATTR OpenTherm::handleInterrupt()
{
	if (isReady())
	{
		if (isSlave && readState() == HIGH) {
		   status = OpenThermStatus::RESPONSE_WAITING;
		}
		else {
			return;
		}
	}

	unsigned long newTs = micros();
	if (status == OpenThermStatus::RESPONSE_WAITING) {
		if ((newTs - responseTimestamp) > 20000) {
			if (readState() == HIGH) {
				status = OpenThermStatus::RESPONSE_START_BIT;
				responseTimestamp = newTs;
			}
			else {
				status = OpenThermStatus::RESPONSE_INVALID;
				responseTimestamp = newTs;
			}
		}
	}
	else if (status == OpenThermStatus::RESPONSE_START_BIT) {
		if ((newTs - responseTimestamp < 750) && readState() == LOW) {
			status = OpenThermStatus::RESPONSE_RECEIVING;
			responseTimestamp = newTs;
			responseBitIndex = 0;
			response = 0;
		}
		else {
			status = OpenThermStatus::RESPONSE_INVALID;
			responseTimestamp = newTs;
		}
	}
	else if (status == OpenThermStatus::RESPONSE_RECEIVING) {
		unsigned long bitDuration = newTs - responseTimestamp;
		if ((newTs - responseTimestamp) > 750 && bitDuration < 1300) { // bitDuration should not bigger than 1500
			if (responseBitIndex < 32) {
				response = (response << 1) | !readState();
				responseTimestamp = newTs;
				responseBitIndex++;
			}
			else { //stop bit
				status = OpenThermStatus::RESPONSE_READY;
				responseTimestamp = newTs;
			}
		}
	}
}

void OpenTherm::process()
{
	noInterrupts();
	OpenThermStatus st = status;
	unsigned long ts = responseTimestamp;
	interrupts();

	if (st == OpenThermStatus::READY) return;
	unsigned long newTs = micros();
	if (st != OpenThermStatus::NOT_INITIALIZED && st != OpenThermStatus::RESPONSE_READY && st != OpenThermStatus::DELAY && (newTs - ts) > 1000000) {
		status = OpenThermStatus::READY;
		responseStatus = OpenThermResponseStatus::TIMEOUT;
		if (processResponseCallback != NULL) {
			processResponseCallback(response, responseStatus);
		}
	}
	else if (st == OpenThermStatus::RESPONSE_INVALID) {
		status = OpenThermStatus::DELAY;
		responseStatus = OpenThermResponseStatus::INVALID;
		if (processResponseCallback != NULL) {
			processResponseCallback(response, responseStatus);
		}
	}
	else if (st == OpenThermStatus::RESPONSE_READY) {
		status = OpenThermStatus::DELAY;
		responseStatus = (isSlave ? isValidRequest(response) : isValidResponse(response)) ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
		if (processResponseCallback != NULL) {
			processResponseCallback(response, responseStatus);
		}
	}
	else if (st == OpenThermStatus::DELAY) {
		if ((newTs - ts) > 100000) {
			status = OpenThermStatus::READY;
		}
	}
}

bool OpenTherm::parity(unsigned long frame) //odd parity
{
	byte p = 0;
	while (frame > 0)
	{
		if (frame & 1) p++;
		frame = frame >> 1;
	}
	return (p & 1);
}

OpenThermMessageType OpenTherm::getMessageType(unsigned long message)
{
	OpenThermMessageType msg_type = static_cast<OpenThermMessageType>((message >> 28) & 7);
	return msg_type;
}

OpenThermMessageID OpenTherm::getDataID(unsigned long frame)
{
	return (OpenThermMessageID)((frame >> 16) & 0xFF);
}

unsigned long OpenTherm::buildRequest(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
	unsigned long request = data;
	if (type == OpenThermMessageType::WRITE_DATA) {
		request |= 1ul << 28;
	}
	request |= ((unsigned long)id) << 16;
	if (parity(request)) request |= (1ul << 31);
	return request;
}

unsigned long OpenTherm::buildResponse(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
	unsigned long response = data;
	response |= ((unsigned long)type) << 28;
	response |= ((unsigned long)id) << 16;
	if (parity(response)) response |= (1ul << 31);
	return response;
}

bool OpenTherm::isValidResponse(unsigned long response)
{
	if (parity(response)) return false;
	byte msgType = (response << 1) >> 29;
	return msgType == READ_ACK || msgType == WRITE_ACK;
}

bool OpenTherm::isValidRequest(unsigned long request)
{
	if (parity(request)) return false;
	byte msgType = (request << 1) >> 29;
	return msgType == READ_DATA || msgType == WRITE_DATA;
}

void OpenTherm::end() {
	if (this->handleInterruptCallback != NULL) {
		detachInterrupt(digitalPinToInterrupt(inPin));
	}
}

const char *OpenTherm::statusToString(OpenThermResponseStatus status)
{
	switch (status) {
		case NONE:	return "NONE";
		case SUCCESS: return "SUCCESS";
		case INVALID: return "INVALID";
		case TIMEOUT: return "TIMEOUT";
		default:	  return "UNKNOWN";
	}
}

const char *OpenTherm::messageTypeToString(OpenThermMessageType message_type)
{
	switch (message_type) {
		case READ_DATA:	   return "READ_DATA";
		case WRITE_DATA:	  return "WRITE_DATA";
		case INVALID_DATA:	return "INVALID_DATA";
		case RESERVED:		return "RESERVED";
		case READ_ACK:		return "READ_ACK";
		case WRITE_ACK:	   return "WRITE_ACK";
		case DATA_INVALID:	return "DATA_INVALID";
		case UNKNOWN_DATA_ID: return "UNKNOWN_DATA_ID";
		default:			  return "UNKNOWN";
	}
}

const char *OpenTherm::stautsToString(OpenThermStatus status)
{
	switch (status) {
		case NOT_INITIALIZED:	   return "NOT_INITIALIZED";
		case READY:	  return "READY";
		case DELAY:	return "DELAY";
		case REQUEST_SENDING:		return "REQUEST_SENDING";
		case RESPONSE_WAITING:		return "RESPONSE_WAITING";
		case RESPONSE_START_BIT:	   return "RESPONSE_START_BIT";
		case RESPONSE_RECEIVING:	return "RESPONSE_RECEIVING";
		case RESPONSE_READY: return "RESPONSE_READY";
		case RESPONSE_INVALID: return "RESPONSE_INVALID";
		default:			  return "UNKNOWN";
	}
}

//building requests

unsigned long OpenTherm::buildSetBoilerStatusRequest(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2) {
	Serial.print("buildSetBoilerStatusRequest: ");	
	Serial.print("enableCentralHeating: "+String(enableCentralHeating)+", ");	
	Serial.print("enableHotWater: "+String(enableHotWater)+", ");
	Serial.print("enableCooling: "+String(enableCooling)+", ");
	Serial.print("enableOutsideTemperatureCompensation: "+String(enableOutsideTemperatureCompensation)+", ");
	Serial.print("enableCentralHeating2: "+String(enableCentralHeating2)+"");	
	Serial.println("");
	unsigned int data = enableCentralHeating | (enableHotWater << 1) | (enableCooling << 2) | (enableOutsideTemperatureCompensation << 3) | (enableCentralHeating2 << 4);
	data <<= 8;
	unsigned long request = buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Status, data);
	Serial.print("request: ("+String(request,HEX)+") HEX");	
	Serial.println("");
	return request;
}

unsigned long OpenTherm::buildSetBoilerTemperatureRequest(float temperature) {
	unsigned int data = temperatureToData(temperature);
	return buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, data);
}

unsigned long OpenTherm::buildGetBoilerTemperatureRequest() {
	return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0);
}

//parsing responses
bool OpenTherm::isFault(unsigned long response) {
	return response & 0x1;
}

bool OpenTherm::isCentralHeatingActive(unsigned long response) {
	return response & 0x2;
}

bool OpenTherm::isHotWaterActive(unsigned long response) {
	return response & 0x4;
}

bool OpenTherm::isFlameOn(unsigned long response) {
	return response & 0x8;
}

bool OpenTherm::isCoolingActive(unsigned long response) {
	return response & 0x10;
}

bool OpenTherm::isDiagnostic(unsigned long response) {
	return response & 0x40;
}

uint16_t OpenTherm::getUInt(const unsigned long response) const {
	const uint16_t u88 = response & 0xffff;
	return u88;
}

float OpenTherm::getFloat(const unsigned long response) const {
	const uint16_t u88 = getUInt(response);
	const float f = (u88 & 0x8000) ? -(0x10000L - u88) / 256.0f : u88 / 256.0f;
	return f;
}

unsigned int OpenTherm::temperatureToData(float temperature) {
	if (temperature < 0) temperature = 0;
	if (temperature > 100) temperature = 100;
	unsigned int data = (unsigned int)(temperature * 256);
	return data;
}

//basic requests

unsigned long OpenTherm::setBoilerStatus(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2) {
	return sendRequest(buildSetBoilerStatusRequest(enableCentralHeating, enableHotWater, enableCooling, enableOutsideTemperatureCompensation, enableCentralHeating2));
}

bool OpenTherm::setBoilerTemperature(float temperature) {
	unsigned long response = sendRequest(buildSetBoilerTemperatureRequest(temperature));
	return isValidResponse(response);
}

float OpenTherm::getBoilerTemperature() {
	unsigned long response = sendRequest(buildGetBoilerTemperatureRequest());
	return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getReturnTemperature() {
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Tret, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

bool OpenTherm::setDHWSetpoint(float temperature) {
    unsigned int data = temperatureToData(temperature);
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TdhwSet, data));
    return isValidResponse(response);
}
    
float OpenTherm::getDHWTemperature() {
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tdhw, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getModulation() {
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::RelModLevel, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getPressure() {
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::CHPressure, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

unsigned char OpenTherm::getFault() {
    return ((sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::ASFflags, 0)) >> 8) & 0xff);
}

const char *OpenTherm::OpenThermMessageIDToString(OpenThermMessageID id) {
  switch (id) {
    case OpenThermMessageID::Status:
      return "Status";
    case OpenThermMessageID::TSet:
      return "TSet";
    case OpenThermMessageID::MConfigMMemberIDcode:
      return "MConfigMMemberIDcode";
    case OpenThermMessageID::SConfigSMemberIDcode:
      return "SConfigSMemberIDcode";
    case OpenThermMessageID::Command:
      return "Command";
    case OpenThermMessageID::ASFflags:
      return "ASFflags";
    case OpenThermMessageID::RBPflags:
      return "RBPflags";
    case OpenThermMessageID::CoolingControl:
      return "CoolingControl";
    case OpenThermMessageID::TsetCH2:
      return "TsetCH2";
    case OpenThermMessageID::TrOverride:
      return "TrOverride";
    case OpenThermMessageID::TSP:
      return "TSP";
    case OpenThermMessageID::TSPindexTSPvalue:
      return "TSPindexTSPvalue";
    case OpenThermMessageID::FHBsize:
      return "FHBsize";
    case OpenThermMessageID::FHBindexFHBvalue:
      return "FHBindexFHBvalue";
    case OpenThermMessageID::MaxRelModLevelSetting:
      return "MaxRelModLevelSetting";
    case OpenThermMessageID::MaxCapacityMinModLevel:
      return "MaxCapacityMinModLevel";
    case OpenThermMessageID::TrSet:
      return "TrSet";
    case OpenThermMessageID::RelModLevel:
      return "RelModLevel";
    case OpenThermMessageID::CHPressure:
      return "CHPressure";
    case OpenThermMessageID::DHWFlowRate:
      return "DHWFlowRate";
    case OpenThermMessageID::DayTime:
      return "DayTime";
    case OpenThermMessageID::Date:
      return "Date";
    case OpenThermMessageID::Year:
      return "Year";
    case OpenThermMessageID::TrSetCH2:
      return "TrSetCH2";
    case OpenThermMessageID::Tr:
      return "Tr";
    case OpenThermMessageID::Tboiler:
      return "Tboiler";
    case OpenThermMessageID::Tdhw:
      return "Tdhw";
    case OpenThermMessageID::Toutside:
      return "Toutside";
    case OpenThermMessageID::Tret:
      return "Tret";
    case OpenThermMessageID::Tstorage:
      return "Tstorage";
    case OpenThermMessageID::Tcollector:
      return "Tcollector";
    case OpenThermMessageID::TflowCH2:
      return "TflowCH2";
    case OpenThermMessageID::Tdhw2:
      return "Tdhw2";
    case OpenThermMessageID::Texhaust:
      return "Texhaust";
    case OpenThermMessageID::TdhwSetUBTdhwSetLB:
      return "TdhwSetUBTdhwSetLB";
    case OpenThermMessageID::MaxTSetUBMaxTSetLB:
      return "MaxTSetUBMaxTSetLB";
    case OpenThermMessageID::HcratioUBHcratioLB:
      return "HcratioUBHcratioLB";
    case OpenThermMessageID::TdhwSet:
      return "TdhwSet";
    case OpenThermMessageID::MaxTSet:
      return "MaxTSet";
    case OpenThermMessageID::Hcratio:
      return "Hcratio";
    case OpenThermMessageID::RemoteOverrideFunction:
      return "RemoteOverrideFunction";
    case OpenThermMessageID::OEMDiagnosticCode:
      return "OEMDiagnosticCode";
    case OpenThermMessageID::BurnerStarts:
      return "BurnerStarts";
    case OpenThermMessageID::CHPumpStarts:
      return "CHPumpStarts";
    case OpenThermMessageID::DHWPumpValveStarts:
      return "DHWPumpValveStarts";
    case OpenThermMessageID::DHWBurnerStarts:
      return "DHWBurnerStarts";
    case OpenThermMessageID::BurnerOperationHours:
      return "BurnerOperationHours";
    case OpenThermMessageID::CHPumpOperationHours:
      return "CHPumpOperationHours";
    case OpenThermMessageID::DHWPumpValveOperationHours:
      return "DHWPumpValveOperationHours";
    case OpenThermMessageID::DHWBurnerOperationHours:
      return "DHWBurnerOperationHours";
    case OpenThermMessageID::OpenThermVersionMaster:
      return "OpenThermVersionMaster";
    case OpenThermMessageID::OpenThermVersionSlave:
      return "OpenThermVersionSlave";
    case OpenThermMessageID::MasterVersion:
      return "MasterVersion";
    case OpenThermMessageID::SlaveVersion:
      return "SlaveVersion";
    default:
      return "ID sconosciuto";
  }
}