#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <ctype.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>

#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Replace with your network credentials
static char const * const ssid = "Die Schmupis";
static char const * const password = "Grosse Wohnung, grosses WLAN!";

#define DebugSerial Serial
#define DataSerial Serial2

static void Serial_reconfigure();

struct ArduFmtBase {};

template<typename T>
struct ArduFmt : public ArduFmtBase
{
    T value;
    int fmt;
    ArduFmt(T value, int fmt) : value(value), fmt(fmt) { }
};

template<typename T>
ArduFmt<T> fmt(T const & value, int f)
{
    return ArduFmt<T> { value, f };
}

template<typename T, typename ... Args>
void debug_print(T const & arg, Args const & ... args)
{   
    if constexpr(std::is_base_of_v<ArduFmtBase, T>) {
        DebugSerial.print(arg.value, arg.fmt);        
    }
    else {
        DebugSerial.print(arg);        
    }

    if constexpr(sizeof...(Args) > 0) {
        debug_print(args...);
    }
    else {
        DebugSerial.println();
    }
}

[[noreturn]] static void _assert(char const * assertion) {
    debug_print("ASSERTION FAILED: ", assertion);
    esp_system_abort(assertion);
}

#define ASSERT(_X)          \
    do {                    \
        if((_X) == 0) {     \
            _assert(#_X);   \
        }                   \
    } while(false)

struct Formatter
{
    char * buffer;
    size_t len;
    size_t index = 0;

    template<size_t N>
    Formatter(char (&buffer)[N]) : 
        buffer(buffer), len(N), index(0)
    {
        this->buffer[0] = 0;
    }

    explicit Formatter(char * buffer, size_t len) : 
        buffer(buffer), len(len), index(0)
    {
        this->buffer[0] = 0;
    }

    bool append(char c)
    {
        if(this->index + 1 < len) {
           this->buffer[this->index] = c;
           this->index += 1;
           this->buffer[this->index] = 0;
           return true;
        } else {
            return false;
        }
    }

    bool print(char const * text)
    {
        while(*text) {
            if(!this->append(*text))
                return false;
            text += 1;
        }
        return true;
    }

    bool print(int ival)
    {
        if(ival == 0) {
            return this->append('0');
        }
        if(ival < 0) {
            if(!this->append('-'))
                return false;
            ival = -ival;
        }

        char buf[20];
        size_t i = 0;
        while(ival > 0) {
            buf[i] = ("0123456789")[ival % 10];            
            ival /= 10;
            i += 1;
        }

        while(i > 0) {
            i -= 1;
            if(!this->append(buf[i]))
                return false;
        }

        return true;
    }

    template<typename T, typename ... Args>
    bool print(T const & arg, Args const & ... args)
    {
        if(!print(arg))
            return false;

        if constexpr(sizeof...(Args) > 0) {
            return this->print(args...);
        }
        return true;
    }
};

struct TcpServerSocket
{
    static constexpr const char * const TAG = "data server";
    static constexpr size_t queue_length = 128;

    int listen_sock = -1;
    int client_sock = -1;
    std::mutex client_sock_lock;
    QueueHandle_t receive_queue = nullptr;

    TcpServerSocket() = default;
    virtual ~TcpServerSocket() = default;

    void start(uint16_t port)
    {
        if(listen_sock != -1) {
            return;
        }

        this->receive_queue = xQueueCreate(queue_length, 1);
        ASSERT(this->receive_queue != nullptr);
        
        char addr_str[128];
        // int keepAlive = 1;
        // int keepIdle = KEEPALIVE_IDLE;
        // int keepInterval = KEEPALIVE_INTERVAL;
        // int keepCount = KEEPALIVE_COUNT;

        struct sockaddr_in dest_addr = {0};
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(port);
        
        this->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT(this->listen_sock >= 0);

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        ESP_LOGI(TAG, "Socket created");

        int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            esp_system_abort("unable to bind socket!");
        }
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);

        err = listen(listen_sock, 1);
        if (err != 0) {
            ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
            esp_system_abort("unable to listen!");
        }

        xTaskCreate(wrapped_tcp_server_task, "tcp server", 4096, (void*)this, 5, nullptr);
    }

    size_t read(char * buffer, size_t len)
    {
        if(this->receive_queue == nullptr) {
            return 0;
        }
        for(size_t i = 0; i < len; i++) {
            if(xQueueReceive(this->receive_queue, &buffer[i], 0) == pdFALSE) {
                return i;
            }
        }
        return len;
    }

    size_t write(char const * buffer, size_t len)
    {
        std::lock_guard<std::mutex> _ { this->client_sock_lock };
        if(this->client_sock >= 0) {
            ssize_t sent = send(this->client_sock, buffer, len, 0);
            if(sent < 0) {
                return 0;
            }
            return static_cast<size_t>(sent);
        }
        else {
            return 0;
        }
    }

    void handle_connection(int sock)
    {
        {
            std::lock_guard<std::mutex> _ { this->client_sock_lock };
            this->client_sock = sock;
        }

        while(true)
        {
            char buffer;
            ssize_t len = recv(sock, &buffer, 1, 0);
            if(len != 1) {
                break;
            }
            // DataSerial.write(&buffer, 1);
            while(xQueueSend(this->receive_queue, &buffer, 100 * portTICK_PERIOD_MS) == pdFALSE)
            {
                // retry!
                // the serial port will eventually have everything sent.
            }
        }

        {
            std::lock_guard<std::mutex> _ { this->client_sock_lock };
            this->client_sock = -1;
        }
    }

    void tcp_server_task()
    {
        while (1) {

            ESP_LOGI(TAG, "Socket listening");

            struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
            socklen_t addr_len = sizeof(source_addr);
            int const sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                esp_system_abort("unable to accept socket!");
            }

            // Set tcp keepalive option
            // setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
            // setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
            // setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
            // setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

            // Convert ip address to string
            char addr_str[128] = "";
            if (source_addr.ss_family == AF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
            }
            
            ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

            this->handle_connection(sock);

            shutdown(sock, 0);
            close(sock);
        }

        close(listen_sock);
    }

    static void wrapped_tcp_server_task(void *pvParameters)
    {
        TcpServerSocket * const server = static_cast<TcpServerSocket *>(pvParameters);
        server->tcp_server_task();
        vTaskDelete(NULL);
    }
};

static char const * parse_configuration_command(char const * cmd, size_t len);

static TcpServerSocket data_server;

static TcpServerSocket conf_server;

static char data_buffer[256];
static char conf_buffer[64];
static size_t conf_cursor = 0;
static bool conf_faulted = false;

void setup()
{
    DebugSerial.begin(115200);

    debug_print("Connecting to WiFi...");

    WiFi.mode(WIFI_STA);    
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(10);
    }

    debug_print("Connected!");
    
    debug_print("Local IP: ", WiFi.localIP());

    data_server.start(23);
    conf_server.start(1337);

    Serial_reconfigure();

    debug_print("Ready.");
}

void loop()
{
    // Fetch data from serial and broadcast:
    while(DataSerial.available() > 0)
    {
        size_t const max_len = DataSerial.available();
        size_t len = DataSerial.readBytes(data_buffer, std::min<size_t>(sizeof data_buffer, max_len));
        if(len > 0) {
            // debug_print(slice(data_buffer, len));
            data_server.write(data_buffer, len);
        }
    }

    // Fetch data from wifi and output:
    {
        size_t const len = data_server.read(data_buffer, sizeof data_buffer);
        if(len > 0) {
            DataSerial.write(data_buffer, len);
            // debug_print(slice(data_buffer, len));
        }
    }
    
    // Fetch configuration commands:
    {
        size_t const len = conf_server.read(data_buffer, sizeof data_buffer);
        for(size_t i = 0; i < len; i++) {
            uint8_t byte = static_cast<uint8_t>(data_buffer[i]);
            if(byte == '\r' or byte == '\n') {
                if(not conf_faulted) {
                    // handle configuration buffer
                    char const * msg = parse_configuration_command(conf_buffer, conf_cursor);
                    conf_server.write(msg, strlen(msg));
                    conf_server.write("\r\n", 2);
                }
                conf_faulted = false;
                conf_cursor = 0;
            }
            else if(conf_cursor < sizeof conf_buffer) {
                conf_buffer[conf_cursor] = byte;
                conf_cursor += 1;
            }
            else {
                conf_faulted = true;
            }
        }
    }
}

static bool str_eq(char const * text, size_t len, char const * what)
{
    for(size_t i = 0; i < len; i++) {
        if(text[i] != what[i])
            return false;
    }
    return (what[len] == 0);
}

static bool parse_uint(char const * text, size_t len, int & value)
{
    if(len == 0) return false; // too short

    value = 0;
    for(size_t i = 0; i < len; i++)
    {
        char const c = text[i];
        if(c < '0' or c > '9') return false; // bad char

        int const digit = c - '0';

        if(value > INT_MAX / 10 - digit) return false; // out of range
        value = 10 * value + digit;
    }

    return true;
}

struct SerialModeMap {
    char const * key;
    int value;
};

static SerialModeMap const serial_mode_map[] = {
    { "5e1", SERIAL_5E1 },
    { "5e2", SERIAL_5E2 },
    { "5n1", SERIAL_5N1 },
    { "5n2", SERIAL_5N2 },
    { "5o1", SERIAL_5O1 },
    { "5o2", SERIAL_5O2 },
    { "6e1", SERIAL_6E1 },
    { "6e2", SERIAL_6E2 },
    { "6n1", SERIAL_6N1 },
    { "6n2", SERIAL_6N2 },
    { "6o1", SERIAL_6O1 },
    { "6o2", SERIAL_6O2 },
    { "7e1", SERIAL_7E1 },
    { "7e2", SERIAL_7E2 },
    { "7n1", SERIAL_7N1 },
    { "7n2", SERIAL_7N2 },
    { "7o1", SERIAL_7O1 },
    { "7o2", SERIAL_7O2 },
    { "8e1", SERIAL_8E1 },
    { "8e2", SERIAL_8E2 },
    { "8n1", SERIAL_8N1 },
    { "8n2", SERIAL_8N2 },
    { "8o1", SERIAL_8O1 },
    { "8o2", SERIAL_8O2 },
};

struct StoredSerialConfig {
    bool online = true;
    int baud_rate = 115200;
    int mode = SERIAL_8N1;
};

static StoredSerialConfig serial_config;

static char const * parse_configuration_command(char const * cmd, size_t len)
{
    static char result_buffer[64];

    size_t split;
    for(split = 0; split < len; split++) {
        if(cmd[split] == '=') {
            break;
        }
    }

    if(split >= len) {
        return "missing '='";
    }

    char const * const value = cmd + split + 1U;
    size_t const vlen = len - split - 1U;

    if(vlen == 0) {
        return "no value.";
    }

    bool const query = (vlen == 1 and value[0] == '?');

    if(str_eq(cmd, split, "baud") or str_eq(cmd, split, "baudrate"))
    {
        if(query) {
            Formatter fmt(result_buffer);
            fmt.print("baud=", serial_config.baud_rate);
            return result_buffer;
        }
        else {
            int baud;
            if(not parse_uint(value, vlen,  baud)) {
                return "bad int.";
            }
            serial_config.baud_rate = baud;
        }
    }
    else if(str_eq(cmd, split, "mode"))
    {
        if(query) {
            Formatter fmt(result_buffer);
            fmt.print("mode=");
            for(auto const & mode : serial_mode_map)
            {
                if(serial_config.mode == mode.value) {
                    fmt.print(mode.key);
                    break;
                }
            }
            return result_buffer;
        }
        else {
            bool ok = false;
            for(auto const & mode : serial_mode_map)
            {
                if(str_eq(value, vlen, mode.key)) {
                    serial_config.mode = mode.value;
                    ok = true;
                    break;
                }
            }
            if(!ok) {
                return "bad mode.";
            }
        }
    }
    else if(str_eq(cmd, split, "online") or str_eq(cmd, split, "enabled"))
    {
        if(query) {
            Formatter fmt(result_buffer);
            fmt.print("online=", serial_config.online ? "yes" : "no");
            return result_buffer;
        }
        else {
            if(str_eq(value, vlen, "yes") or str_eq(value, vlen, "on") or str_eq(value, vlen, "true") or str_eq(value, vlen, "1")) {
                serial_config.online = true;
            }
            else if(str_eq(value, vlen, "no") or str_eq(value, vlen, "off") or str_eq(value, vlen, "false") or str_eq(value, vlen, "0")) {
                serial_config.online = false;
            }
            else {
                return "bad bool.";
            }
        }
    }
    else
    {
        return "bad key.";
    }

    Serial_reconfigure();

    return "ok.";
}

static void Serial_reconfigure()
{
    DataSerial.end();

    if(serial_config.online) {
        debug_print("reconfigure: online, baud=", fmt(serial_config.baud_rate, DEC), ", mode=", fmt(serial_config.mode, HEX));
        DataSerial.begin(serial_config.baud_rate, serial_config.mode);
    }
    else {
        debug_print("reconfigure: offline");
    }
}
