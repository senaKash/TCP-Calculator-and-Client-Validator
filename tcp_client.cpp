// TCP Client Validator (client.cpp)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

constexpr int MAX_EVENTS = 1000; // Максимальное количество событий для epoll

// Устанавливает неблокирующий режим для файлового дескриптора
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Возвращает приоритет оператора
int precedence(char op) {
    if (op == '+' || op == '-') return 1;
    if (op == '*' || op == '/') return 2;
    return 0;
}

// Применяет оператор op к значениям a и b
long apply_op(long a, long b, char op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            return b == 0 ? 0 : a / b;  // в клиенте на деление на ноль — 0
    }
    throw std::runtime_error("Unknown operator");
}

// Локальное вычисление выражения (shunting‑yard)
long evaluate(const std::string& s) {
    std::stack<long> values;  // стек для чисел
    std::stack<char> ops;     // стек для операторов

    for (size_t i = 0; i < s.size(); ) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            // читаем целое число
            long val = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                val = val * 10 + (s[i++] - '0');
            }
            values.push(val);
        }
        else {
            // текущий символ — оператор
            char op = s[i++];
            // пока в стеке ops есть оператор с приоритетом >= текущего
            while (!ops.empty() && precedence(ops.top()) >= precedence(op)) {
                long b = values.top(); values.pop();
                long a = values.top(); values.pop();
                char top_op = ops.top(); ops.pop();
                values.push(apply_op(a, b, top_op));
            }
            ops.push(op);
        }
    }

    // применяем оставшиеся операторы
    while (!ops.empty()) {
        long b = values.top(); values.pop();
        long a = values.top(); values.pop();
        char top_op = ops.top(); ops.pop();
        values.push(apply_op(a, b, top_op));
    }

    if (values.empty()) throw std::runtime_error("Empty expression");
    return values.top();
}

// Генерация случайного арифметического выражения из n чисел
std::string build_expression(int n, std::mt19937 &rng) {
    std::uniform_int_distribution<int> dist_num(1, 10); // числа от 1 до 10
    std::uniform_int_distribution<int> dist_op(0, 3);   // операторы +, -, *, /
    const char ops[4] = {'+', '-', '*', '/'};
    std::ostringstream ss;
    for (int i = 0; i < n; ++i) {
        ss << dist_num(rng);
        if (i + 1 < n) ss << ops[dist_op(rng)];
    }
    return ss.str();
}

// Структура для хранения состояния одного соединения
struct Connection {
    std::string expr;                 // исходное выражение
    std::vector<std::string> fragments; // фрагменты для отправки
    size_t frag_idx = 0;              // индекс текущего фрагмента
    size_t frag_offset = 0;           // смещение внутри фрагмента
    std::string in_buf;               // буфер входящих данных
    long expected;                    // ожидаемый результат
};

int main(int argc, char* argv[]) {
    // Проверяем аргументы: n, connections, адрес и порт сервера
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <n> <connections> <server_addr> <server_port>\n";
        return 1;
    }
    int n = std::stoi(argv[1]);            // количество чисел
    int connections = std::stoi(argv[2]); // число параллельных сессий
    const char* server_addr = argv[3];     // адрес сервера
    int server_port = std::stoi(argv[4]);  // порт сервера

    // Создаем epoll‑демон
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return 1; }

    // Инициализируем генератор случайных чисел
    std::mt19937 rng(static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    std::unordered_map<int, Connection> conns;     // мапа fd -> Connection
    std::vector<epoll_event> events(MAX_EVENTS);   // массив для epoll_wait

    // Инициализируем все соединения
    for (int i = 0; i < connections; ++i) {
        Connection c;
        c.expr = build_expression(n, rng);
        c.expected = evaluate(c.expr);
        std::cout << "[Conn " << i << "] Expr: " << c.expr
                  << " Expected: " << c.expected << std::endl;

        // Добавляем пробел в конце как разделитель
        std::string msg = c.expr + ' ';

        // Фрагментация строки на случайные куски
        int pos = 0;
        while (pos < (int)msg.size()) {
            int max_len = msg.size() - pos;
            int len = std::uniform_int_distribution<int>(1, max_len)(rng);
            c.fragments.push_back(msg.substr(pos, len));
            pos += len;
        }

        // Создаем неблокирующий сокет и подключаемся
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        set_nonblocking(fd);
        sockaddr_in serv{};
        serv.sin_family = AF_INET;
        inet_pton(AF_INET, server_addr, &serv.sin_addr);
        serv.sin_port = htons(server_port);
        connect(fd, (sockaddr*)&serv, sizeof(serv));

        // Регистрируем fd в epoll на чтение и запись
        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);

        conns[fd] = std::move(c); // сохраняем состояние
        std::cout << "[Conn " << i << "] Opened fd=" << fd << std::endl;
    }

    int active = conns.size(); // сколько еще активных соединений

    // Основной цикл обработки событий
    while (active > 0) {
        int n_events = epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);
        if (n_events < 0 && errno == EINTR) continue;

        for (int i = 0; i < n_events; ++i) {
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            Connection &c = it->second;

            // Отправка фрагментов при готовности записи
            if ((evs & EPOLLOUT) && c.frag_idx < c.fragments.size()) {
                while (c.frag_idx < c.fragments.size()) {
                    const std::string &frag = c.fragments[c.frag_idx];
                    const char* data = frag.data() + c.frag_offset;
                    size_t left = frag.size() - c.frag_offset;
                    ssize_t sent = send(fd, data, left, 0);
                    if (sent > 0) {
                        c.frag_offset += sent;
                        if (c.frag_offset == frag.size()) {
                            c.frag_idx++;
                            c.frag_offset = 0;
                        }
                    } else if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else {
                        close(fd);
                        conns.erase(it);
                        active--;
                        goto next_fd;
                    }
                }
                // Если все фрагменты отправлены — отключаем EPOLLOUT
                if (c.frag_idx == c.fragments.size()) {
                    epoll_event mod{};
                    mod.data.fd = fd;
                    mod.events = EPOLLIN | EPOLLET;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &mod);
                }
            }

            // Приём ответа при готовности чтения
            if (evs & EPOLLIN) {
                char buf[64];
                while (true) {
                    ssize_t count = recv(fd, buf, sizeof(buf), 0);
                    if (count > 0) {
                        c.in_buf.append(buf, count);
                    } else if (count == 0 ||
                              (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                        break;
                    } else {
                        break;
                    }
                }
                // Проверяем разделитель (пробел)
                size_t pos;
                if ((pos = c.in_buf.find(' ')) != std::string::npos) {
                    std::string resp = c.in_buf.substr(0, pos);
                    long server_res = std::stol(resp);
                    if (server_res != c.expected) {
                        std::cerr << "Mismatch! Expr: " << c.expr
                                  << ", Server: " << server_res
                                  << ", Expected: " << c.expected << std::endl;
                    } else {
                        std::cout << "Match! Expr: " << c.expr
                                  << ", Result: " << server_res << std::endl;
                    }
                    close(fd);
                    conns.erase(it);
                    active--;
                }
            }
        next_fd:;
        }
    }

    return 0;
}
