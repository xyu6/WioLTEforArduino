#include <Arduino.h>
#include <stdio.h>
#include <limits.h>
#include "wiolte-driver.h"

//#define DEBUG

#ifdef DEBUG
#define DEBUG_PRINT(str)			SerialUSB.print(str)
#define DEBUG_PRINTLN(str)			SerialUSB.println(str)
#else
#define DEBUG_PRINT(str)
#define DEBUG_PRINTLN(str)
#endif

#define RET_OK(val)					(val)
#define RET_ERR(val)				(ErrorOccured(__LINE__, val))

#define CONNECT_ID_NUM				(12)
#define POLLING_INTERVAL			(100)

#define LINEAR_SCALE(val, inMin, inMax, outMin, outMax)	(((val) - (inMin)) / ((inMax) - (inMin)) * ((outMax) - (outMin)) + (outMin))

////////////////////////////////////////////////////////////////////////////////////////
// Helper functions

static void PinModeAndDefault(int pin, WiringPinMode mode)
{
	pinMode(pin, mode);
}

static void PinModeAndDefault(int pin, WiringPinMode mode, int value)
{
	pinMode(pin, mode);
	if (mode == OUTPUT) digitalWrite(pin, value);
}

static int HexToInt(char c)
{
	if ('0' <= c && c <= '9') return c - '0';
	else if ('a' <= c && c <= 'f') return c - 'a' + 10;
	else if ('A' <= c && c <= 'F') return c - 'A' + 10;
	else return -1;
}

static bool ConvertHexToBytes(const char* hex, byte* data, int dataSize)
{
	int high;
	int low;

	for (int i = 0; i < dataSize; i++) {
		high = HexToInt(hex[i * 2]);
		low = HexToInt(hex[i * 2 + 1]);
		if (high < 0 || low < 0) return false;
		data[i] = high * 16 + low;
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////
// WioLTE

bool WioLTE::ErrorOccured(int lineNumber, bool value)
{
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(lineNumber);

	return value;
}

int WioLTE::ErrorOccured(int lineNumber, int value)
{
	DEBUG_PRINT("ERROR! ");
	DEBUG_PRINTLN(lineNumber);

	return value;
}

bool WioLTE::Reset()
{
	digitalWrite(RESET_MODULE_PIN, LOW);
	delay(200);
	digitalWrite(RESET_MODULE_PIN, HIGH);
	delay(300);

	Stopwatch sw;
	sw.Start();
	while (_Module.WaitForResponse("RDY", 100) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return false;
	}
	DEBUG_PRINTLN("");

	return true;
}

bool WioLTE::TurnOn()
{
	delay(100);
	digitalWrite(PWR_KEY_PIN, HIGH);
	delay(200);
	digitalWrite(PWR_KEY_PIN, LOW);

	Stopwatch sw;
	sw.Start();
	while (IsBusy()) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 5000) return false;
		delay(100);
	}
	DEBUG_PRINTLN("");

	sw.Start();
	while (_Module.WaitForResponse("RDY", 100) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return false;
	}
	DEBUG_PRINTLN("");

	return true;
}

int WioLTE::GetFirstIndexOfReceivedSMS()
{
	const char* parameter;
	const char* hex;
	ArgumentParser parser;

	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=0", "OK", 500) == NULL) return -1;

	_Module.WriteCommand("AT+CMGL=4");	// ALL

	int messageIndex = -1;
	while (true) {
		parameter = _Module.WaitForResponse("OK", 500, "+CMGL: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH));
		if (parameter == NULL) return -1;
		if (strcmp(parameter, "OK") == 0) break;
		if (messageIndex < 0) {
			parser.Parse(parameter);
			if (parser.Size() != 4) return -1;
			messageIndex = atoi(parser[0]);
		}

		const char* hex = _Module.WaitForResponse(NULL, 500, "");
		if (hex == NULL) return -1;
	}

	return messageIndex < 0 ? -2 : messageIndex;
}

WioLTE::WioLTE() : _Module(), _Led(1, RGB_LED_PIN)
{
}

void WioLTE::Init()
{
	// Power supply
	PinModeAndDefault(MODULE_PWR_PIN, OUTPUT, LOW);
	PinModeAndDefault(ANT_PWR_PIN, OUTPUT, LOW);
	PinModeAndDefault(ENABLE_VCCB_PIN, OUTPUT, LOW);

	// Turn on/off Pins
	PinModeAndDefault(PWR_KEY_PIN, OUTPUT, LOW);
	PinModeAndDefault(RESET_MODULE_PIN, OUTPUT, HIGH);

	// Status Indication Pins
	PinModeAndDefault(STATUS_PIN, INPUT);

	// GPIO Pins
	PinModeAndDefault(WAKEUP_IN_PIN, OUTPUT, LOW);
	PinModeAndDefault(WAKEUP_DISABLE_PIN, OUTPUT, HIGH);
	//PinModeAndDefault(AP_READY_PIN, OUTPUT);  // NOT use
  
	_Module.Init();
	_Led.begin();
}

void WioLTE::LedSetRGB(byte red, byte green, byte blue)
{
	_Led.WS2812SetRGB(0, red, green, blue);
	_Led.WS2812Send();
}

void WioLTE::PowerSupplyLTE(bool on)
{
	digitalWrite(MODULE_PWR_PIN, on ? HIGH : LOW);
}

void WioLTE::PowerSupplyGNSS(bool on)
{
	digitalWrite(ANT_PWR_PIN, on ? HIGH : LOW);
}

void WioLTE::PowerSupplyGrove(bool on)
{
	digitalWrite(ENABLE_VCCB_PIN, on ? HIGH : LOW);
}

bool WioLTE::IsBusy() const
{
	return digitalRead(STATUS_PIN) ? true : false;
}

bool WioLTE::TurnOnOrReset()
{
	if (!IsBusy()) {
		if (!Reset()) return RET_ERR(false);
	}
	else {
		if (!TurnOn()) return RET_ERR(false);
	}

	Stopwatch sw;
	sw.Start();
	while (_Module.WriteCommandAndWaitForResponse("AT", "OK", 500) == NULL) {
		DEBUG_PRINT(".");
		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false);
	}
	DEBUG_PRINTLN("");

	if (_Module.WriteCommandAndWaitForResponse("ATE0", "OK", 500) == NULL) return RET_ERR(false);
	if (_Module.WriteCommandAndWaitForResponse("AT+QURCCFG=\"urcport\",\"uart1\"", "OK", 500) == NULL) return RET_ERR(false);

	sw.Start();
	while (true) {
		_Module.WriteCommand("AT+CPIN?");
		const char* response = _Module.WaitForResponse("OK", 5000, "+CME ERROR: ", ModuleSerial::WFR_START_WITH);
		if (response == NULL) return RET_ERR(false);
		if (strcmp(response, "OK") == 0) break;
		if (sw.ElapsedMilliseconds() >= 10000) return RET_ERR(false);
		delay(POLLING_INTERVAL);
	}

	return RET_OK(true);
}

int WioLTE::GetReceivedSignalStrength()
{
	const char* parameter;

	_Module.WriteCommand("AT+CSQ");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CSQ: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(INT_MIN);

	ArgumentParser parser;
	parser.Parse(parameter);
	if (parser.Size() != 2) return RET_ERR(INT_MIN);
	int rssi = atoi(parser[0]);

	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(INT_MIN);

	if (rssi == 0) return RET_OK(-113);
	else if (rssi == 1) return RET_OK(-111);
	else if (2 <= rssi && rssi <= 30) return RET_OK(LINEAR_SCALE((double)rssi, 2, 30, -109, -53));
	else if (rssi == 31) return RET_OK(-51);
	else if (rssi == 99) return RET_OK(-999);
	else if (rssi == 100) return RET_OK(-116);
	else if (rssi == 101) return RET_OK(-115);
	else if (102 <= rssi && rssi <= 190) return RET_OK(LINEAR_SCALE((double)rssi, 102, 190, -114, -26));
	else if (rssi == 191) return RET_OK(-25);
	else if (rssi == 199) return RET_OK(-999);
	
	return RET_OK(-999);
}

bool WioLTE::GetTime(struct tm* tim)
{
	const char* parameter;

	_Module.WriteCommand("AT+CCLK?");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CCLK: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false);

	if (strlen(parameter) != 19) return RET_ERR(false);
	if (parameter[0] != '"') return RET_ERR(false);
	if (parameter[3] != '/') return RET_ERR(false);
	if (parameter[6] != '/') return RET_ERR(false);
	if (parameter[9] != ',') return RET_ERR(false);
	if (parameter[12] != ':') return RET_ERR(false);
	if (parameter[15] != ':') return RET_ERR(false);
	if (parameter[18] != '"') return RET_ERR(false);

	int yearOffset = atoi(&parameter[1]);
	tim->tm_year = (yearOffset >= 80 ? 1900 : 2000) + yearOffset;
	tim->tm_mon = atoi(&parameter[4]) - 1;
	tim->tm_mday = atoi(&parameter[7]);
	tim->tm_hour = atoi(&parameter[10]);
	tim->tm_min = atoi(&parameter[13]);
	tim->tm_sec = atoi(&parameter[16]);
	tim->tm_wday = 0;
	tim->tm_yday = 0;
	tim->tm_isdst = -1;

	return RET_OK(true);
}

bool WioLTE::SendSMS(const char* dialNumber, const char* message)
{
	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=1", "OK", 500) == NULL) return RET_ERR(false);

	char* str = (char*)alloca(9 + strlen(dialNumber) + 1 + 1);
	sprintf(str, "AT+CMGS=\"%s\"", dialNumber);
	_Module.WriteCommand(str);
	if (_Module.WaitForResponse(NULL, 500, "> ", ModuleSerial::WFR_WITHOUT_DELIM) == NULL) return RET_ERR(false);
	_Module.Write(message);
	_Module.Write("\x1a");
	if (_Module.WaitForResponse("OK", 120000) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

int WioLTE::ReceiveSMS(char* message, int messageSize)
{
	const char* parameter;
	const char* hex;

	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_OK(0);
	if (messageIndex < 0) return RET_ERR(-1);
	if (messageIndex > 999999) return RET_ERR(-1);

	if (_Module.WriteCommandAndWaitForResponse("AT+CMGF=0", "OK", 500) == NULL) return RET_ERR(-1);

	char* str = (char*)alloca(8 + 6 + 1);
	sprintf(str, "AT+CMGR=%d", messageIndex);
	_Module.WriteCommand(str);

	parameter = _Module.WaitForResponse(NULL, 500, "+CMGR: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH));
	if (parameter == NULL) return RET_ERR(-1);

	hex = _Module.WaitForResponse(NULL, 500, "");
	if (hex == NULL) return RET_ERR(-1);
	int hexSize = strlen(hex);
	if (hexSize % 2 != 0) return RET_ERR(-1);
	int dataSize = hexSize / 2;
	byte* data = (byte*)alloca(dataSize);
	if (!ConvertHexToBytes(hex, data, dataSize)) return RET_ERR(-1);
	byte* dataEnd = &data[dataSize];

	// http://www.etsi.org/deliver/etsi_gts/03/0340/05.03.00_60/gsmts_0340v050300p.pdf
	// http://www.etsi.org/deliver/etsi_gts/03/0338/05.00.00_60/gsmts_0338v050000p.pdf
	byte* smscInfoSize = data;
	byte* tpMti = smscInfoSize + 1 + *smscInfoSize;
	if (tpMti >= dataEnd) return RET_ERR(-1);
	if ((*tpMti & 0xc0) != 0) return RET_ERR(-1);	// SMS-DELIVER
	byte* tpOaSize = tpMti + 1;
	if (tpOaSize >= dataEnd) return RET_ERR(-1);
	byte* tpPid = tpOaSize + 2 + *tpOaSize / 2 + *tpOaSize % 2;
	if (tpPid >= dataEnd) return RET_ERR(-1);
	byte* tpDcs = tpPid + 1;
	if (tpDcs >= dataEnd) return RET_ERR(-1);
	byte* tpScts = tpDcs + 1;
	if (tpScts >= dataEnd) return RET_ERR(-1);
	byte* tpUdl = tpScts + 7;
	if (tpUdl >= dataEnd) return RET_ERR(-1);
	byte* tpUd = tpUdl + 1;
	if (tpUd >= dataEnd) return RET_ERR(-1);

	if (messageSize < *tpUdl + 1) return RET_ERR(-1);
	for (int i = 0; i < *tpUdl; i++) {
		int offset = i - i / 8;
		int shift = i % 8;
		if (shift == 0) {
			message[i] = tpUd[offset] & 0x7f;
		}
		else {
			message[i] = tpUd[offset] * 256 + tpUd[offset - 1] << shift >> 8 & 0x7f;
		}
	}
	message[*tpUdl] = '\0';

	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(-1);

	return RET_OK(*tpUdl);
}

bool WioLTE::DeleteReceivedSMS()
{
	int messageIndex = GetFirstIndexOfReceivedSMS();
	if (messageIndex == -2) return RET_ERR(false);
	if (messageIndex < 0) return RET_ERR(false);
	if (messageIndex > 999999) return RET_ERR(false);

	char* str = (char*)alloca(8 + 6 + 1);
	sprintf(str, "AT+CMGD=%d", messageIndex);
	if (_Module.WriteCommandAndWaitForResponse(str, "OK", 500) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

bool WioLTE::Activate(const char* accessPointName, const char* userName, const char* password)
{
	const char* parameter;

	_Module.WriteCommand("AT+CREG?");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CREG: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false);
	if (strcmp(parameter, "0,1") != 0 && strcmp(parameter, "0,3") != 0) return RET_ERR(false);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false);

	_Module.WriteCommand("AT+CGREG?");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CGREG: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false);
	if (strcmp(parameter, "0,1") != 0) return RET_ERR(false);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false);

	_Module.WriteCommand("AT+CEREG?");
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+CEREG: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false);
	if (strcmp(parameter, "0,1") != 0) return RET_ERR(false);
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(false);

	char* str = (char*)alloca(15 + strlen(accessPointName) + 3 + strlen(userName) + 3 + strlen(password) + 3 + 1);
	sprintf(str, "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",1", accessPointName, userName, password);
	if (_Module.WriteCommandAndWaitForResponse(str, "OK", 500) == NULL) return RET_ERR(false);

	if (_Module.WriteCommandAndWaitForResponse("AT+QIACT=1", "OK", 150000) == NULL) return RET_ERR(false);

	if (_Module.WriteCommandAndWaitForResponse("AT+QIACT?", "OK", 150000) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

bool WioLTE::SyncTime(const char* host)
{
	const char* parameter;

	char* str = (char*)alloca(11 + strlen(host) + 1 + 1);
	sprintf(str, "AT+QNTP=1,\"%s\"", host);
	if (_Module.WriteCommandAndWaitForResponse(str, "OK", 500) == NULL) return RET_ERR(false);
	if ((parameter = _Module.WaitForResponse(NULL, 125000, "+QNTP: ", (ModuleSerial::WaitForResponseFlag)(ModuleSerial::WFR_START_WITH | ModuleSerial::WFR_REMOVE_START_WITH))) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

int WioLTE::SocketOpen(const char* host, int port, SocketType type)
{
	if (host == NULL || host[0] == '\0') return RET_ERR(-1);
	if (port < 0 || 65535 < port) return RET_ERR(-1);

	const char* typeStr;
	switch (type) {
	case SOCKET_TCP:
		typeStr = "TCP";
		break;
	case SOCKET_UDP:
		typeStr = "UDP";
		break;
	default:
		return RET_ERR(-1);
	}

	bool connectIdUsed[CONNECT_ID_NUM];
	for (int i = 0; i < CONNECT_ID_NUM; i++) connectIdUsed[i] = false;

	_Module.WriteCommand("AT+QISTATE?");
	const char* response;
	ArgumentParser parser;
	do {
		if ((response = _Module.WaitForResponse("OK", 10000, "+QISTATE: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1);
		if (strncmp(response, "+QISTATE: ", 10) == 0) {
			parser.Parse(&response[10]);
			if (parser.Size() >= 1) {
				int connectId = atoi(parser[0]);
				if (connectId < 0 || CONNECT_ID_NUM <= connectId) return RET_ERR(-1);
				connectIdUsed[connectId] = true;
			}
		}

	} while (strcmp(response, "OK") != 0);

	int connectId;
	for (connectId = 0; connectId < CONNECT_ID_NUM; connectId++) {
		if (!connectIdUsed[connectId]) break;
	}
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1);

	char* str = (char*)alloca(12 + 2 + 2 + 3 + 3 + strlen(host) + 2 + 5 + 1);
	sprintf(str, "AT+QIOPEN=1,%d,\"%s\",\"%s\",%d", connectId, typeStr, host, port);
	if (_Module.WriteCommandAndWaitForResponse(str, "OK", 150000) == NULL) return RET_ERR(-1);
	char* str2 = (char*)alloca(9 + 2 + 2 + 1);
	sprintf(str2, "+QIOPEN: %d,0", connectId);
	if (_Module.WaitForResponse(str2, 150000) == NULL) return RET_ERR(-1);

	return RET_OK(connectId);
}

bool WioLTE::SocketSend(int connectId, const byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false);
	if (dataSize > 1460) return RET_ERR(false);

	char* str = (char*)alloca(10 + 2 + 1 + 4 + 1);
	sprintf(str, "AT+QISEND=%d,%d", connectId, dataSize);
	_Module.WriteCommand(str);
	if (_Module.WaitForResponse(NULL, 500, "> ", ModuleSerial::WFR_WITHOUT_DELIM) == NULL) return RET_ERR(false);
	_Module.Write(data, dataSize);
	if (_Module.WaitForResponse("SEND OK", 5000) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

bool WioLTE::SocketSend(int connectId, const char* data)
{
	return SocketSend(connectId, (const byte*)data, strlen(data));
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(-1);

	char* str2 = (char*)alloca(8 + 2 + 1);
	sprintf(str2, "AT+QIRD=%d", connectId);
	_Module.WriteCommand(str2);
	const char* parameter;
	if ((parameter = _Module.WaitForResponse(NULL, 500, "+QIRD: ", ModuleSerial::WFR_START_WITH)) == NULL) return RET_ERR(-1);
	int dataLength = atoi(&parameter[7]);
	if (dataLength >= 1) {
		if (dataLength > dataSize) return RET_ERR(-1);
		if (_Module.Read(data, dataLength, 500) != dataLength) return RET_ERR(-1);
	}
	if (_Module.WaitForResponse("OK", 500) == NULL) return RET_ERR(-1);

	return RET_OK(dataLength);
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize)
{
	int dataLength = SocketReceive(connectId, (byte*)data, dataSize - 1);
	if (dataLength >= 0) data[dataLength] = '\0';

	return dataLength;
}

int WioLTE::SocketReceive(int connectId, byte* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}

int WioLTE::SocketReceive(int connectId, char* data, int dataSize, long timeout)
{
	Stopwatch sw;
	sw.Start();
	int dataLength;
	while ((dataLength = SocketReceive(connectId, data, dataSize)) == 0) {
		if (sw.ElapsedMilliseconds() >= timeout) return 0;
		delay(POLLING_INTERVAL);
	}
	return dataLength;
}

bool WioLTE::SocketClose(int connectId)
{
	if (connectId >= CONNECT_ID_NUM) return RET_ERR(false);

	char* str = (char*)alloca(11 + 2 + 1);
	sprintf(str, "AT+QICLOSE=%d", connectId);
	if (_Module.WriteCommandAndWaitForResponse(str, "OK", 10000) == NULL) return RET_ERR(false);

	return RET_OK(true);
}

////////////////////////////////////////////////////////////////////////////////////////
