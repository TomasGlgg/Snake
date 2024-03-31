#include <ncurses.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/time.h>

#define msleep(msec) usleep(msec*1000)
#define putchar(x, y, sym) mvaddch(y, x, sym)

#define MAX_SNAKE_COUNT 100  // < 2^8
#define DEFAULT_SNAKE_LENGTH 20
#define START_X 5  // first snake X offset


enum DIRECTION {
    DOWN,
    LEFT,
    UP,
    RIGHT
};

enum PROTOCOL {
    DIRECTION,
    APPLE,
    LOSS,
    CONNECTION,
    QUIT
};

enum COLORS {
    _,
    COLOR_YOU,
    COLOR_OPPONENT,
    COLOR_APPLE
};

struct Snake {
    int online;  // TRUE or FALSE
    int8_t live;  // TRUE or FALSE
    uint8_t id;  // snake id in network, index in server mode
    uint16_t length;
    uint8_t direction;
    uint8_t last_direction;
    uint16_t* coordinates;  // x1 y1 x2 y2 ...
    int sd;
};


struct Snake snakes[MAX_SNAKE_COUNT] = {0};  // snake at index 0 - we
int PLAYING = TRUE;
int w, h;  // window size
uint16_t apple_x, apple_y;
int is_server = -1;  // connection mode (client / server)
int sd = -1;  // server descriptor (if in client mode)
uint8_t snake_id;
uint16_t delay = 200;
struct timeval last_render_time;


struct InitPkt {
    uint8_t new_snake_id;
    uint8_t alive_snakes;

    uint16_t new_x;
    uint16_t new_y;

    uint16_t apple_x;
    uint16_t apple_y;

    uint16_t delay;
    uint16_t sync;
}__attribute__((packed));

struct NewApplePkt {
    uint8_t magic;
    uint16_t apple_x;
    uint16_t apple_y;
}__attribute__((packed));

// server -> client
struct DirectionPktServer {
    uint8_t magic;
    uint8_t id;
    uint8_t direction;
}__attribute__((packed));

// client -> server
struct DirectionPkt {
    uint8_t magic;
    uint8_t direction;
}__attribute__((packed));

struct LossPkt {
    uint8_t magic;
    uint8_t id;
}__attribute__((packed));

struct QuitPkt {
    uint8_t magic;
    uint8_t id;
}__attribute__((packed));

struct ConnectionPkt {
    uint8_t magic;
    uint8_t id;
    uint16_t x;
    uint16_t y;
}__attribute__((packed));

void sendall_pkt_with_exception(void* pkt, int size, int exception_id) {
    for (int i = 1; i<MAX_SNAKE_COUNT; i++) {
        if (snakes[i].online && (int)snakes[i].id != exception_id) {
            send(snakes[i].sd, pkt, size, 0);
        }
    }
}

void sendall_pkt(void* pkt, int size) {
    sendall_pkt_with_exception(pkt, size, -1);
}

void sendall_direction_pkt(struct DirectionPktServer packet) {
    packet.magic = DIRECTION;
    sendall_pkt_with_exception(&packet, sizeof(struct DirectionPktServer), packet.id);
}

int id_by_sd(int sd) {
    for (int i = 1; i<MAX_SNAKE_COUNT; i++) if (snakes[i].online && snakes[i].sd == sd) return i;
    return -1;
}

void close_connections() {  // server
    for (int i = 1; i<MAX_SNAKE_COUNT; i++) {
        if (snakes[i].online) close(snakes[i].id);
    }
}

void quit(int num) {
    PLAYING = FALSE;
    endwin();

    uint8_t magic = QUIT;
    if (is_server) {
        sendall_pkt(&magic, 1);
        close_connections();
    } else {
        send(sd, &magic, 1, 0);
        close(sd);
    }


    printf("Interrupt: %d\n", num);
    exit(0);
}

void show_snake(struct Snake* snake) {
    int color = snake->id == snake_id ? COLOR_PAIR(COLOR_YOU) : COLOR_PAIR(COLOR_OPPONENT);
    attron(color);
    for (int i = snake->length-1; i>0; i--)
        putchar(snake->coordinates[i*2] + 1, snake->coordinates[i*2 + 1] + 1, '#');
    attroff(color);
}

void init_snake(struct Snake* snake) {
    snake->online = TRUE;
    snake->live = TRUE;
    snake->direction = DOWN;
    snake->length = DEFAULT_SNAKE_LENGTH;
    snake->coordinates = malloc(DEFAULT_SNAKE_LENGTH * 2 * sizeof(uint16_t));
}

struct Snake* init_snake_by_id(int id) {
    struct Snake* snake = snakes + id;
    init_snake(snake);
    snake->id = id;
    return snake;
}

int find_free_slot() {
    for (int i = 1; i<MAX_SNAKE_COUNT; i++) if (!snakes[i].online) return i;
    return -1;
}

struct Snake* new_snake() {
    int id = find_free_slot();
    struct Snake* snake = init_snake_by_id(id);
    return snake;
}

struct Snake* generate_snake() {
    struct Snake* snake = new_snake();
    for (int start_x = START_X; start_x < h; start_x++) {
        int exists = FALSE;
        for (int i = 0; i<snake->length; i++) {
            if (block_exists(start_x, i)) {
                exists = TRUE;
                continue;
            }
            snake->coordinates[i*2] = start_x;
            snake->coordinates[i*2 + 1] = i;
        }
        if (!exists) {
            return snake;
        }
    }
    quit(5);
}

void generate_snake_by_coordinates(int id, int x, int y) {
    struct Snake* snake = init_snake_by_id(id);
    for (int i = 0; i<snake->length; i++) {
        snake->coordinates[i*2] = x;
        snake->coordinates[i*2 + 1] = y + i;
    }
    show_snake(snake);
}

void generate_my_snake(int x, int y) {
    generate_snake_by_coordinates(snake_id, x, y);
}

void delete_snake(int id) {
    struct Snake* snake = snakes + id;
    snake->live = FALSE;
    for (int i = 0; i<snake->length; i++) putchar(snake->coordinates[i*2] + 1, snake->coordinates[i*2 + 1] + 1, ' ');
    free(snake->coordinates);
}

int block_exists(int x, int y) {
    struct Snake* snake;
    for (int i = 0; i<MAX_SNAKE_COUNT; i++) {
        snake = snakes + i;
        if (snake->live) {
            for (int j = 0; j<snake->length; j++) {
                if (x == snake->coordinates[j*2] && y == snake->coordinates[j*2 + 1]) return TRUE;
            }
        }
    }
    return FALSE;
}

void show_apple() {
    attron(COLOR_PAIR(COLOR_APPLE));
    putchar(apple_x + 1, apple_y + 1, '%');
    attroff(COLOR_PAIR(COLOR_APPLE));
}

void generate_apple() {
    do {
        apple_x = rand() % w;
        apple_y = rand() % h;
    } while (block_exists(apple_x, apple_y));
    show_apple();
}

void sendall_apple() {  // server
    struct NewApplePkt pkt;
    pkt.magic = APPLE;
    pkt.apple_x = apple_x;
    pkt.apple_y = apple_y;
    sendall_pkt(&pkt, sizeof(struct NewApplePkt));
}

void send_loss() {  // client
    uint8_t magic = LOSS;
    send(sd, &magic, 1, 0);
}

void sendall_loss(int id) {  // server
    struct LossPkt pkt;
    pkt.magic = LOSS;
    pkt.id = id;
    sendall_pkt_with_exception(&pkt, sizeof(struct LossPkt), id);
}

void sendall_quit(int id) {
    struct QuitPkt pkt;
    pkt.magic = QUIT;
    pkt.id = id;
    sendall_pkt(&pkt, sizeof(struct QuitPkt));
}

void render() {
    struct Snake* snake;

    while (PLAYING) {
        for (int i = 0; i<MAX_SNAKE_COUNT; i++) {
            snake = snakes + i;
            if (snake->live) {
                uint16_t x = snake->coordinates[(snake->length - 1)*2];
                uint16_t y = snake->coordinates[(snake->length - 1)*2 + 1];
                if (snake->direction == DOWN) y++;
                else if (snake->direction == LEFT) x--;
                else if (snake->direction == UP) y--;
                else if (snake->direction == RIGHT) x++;
                snake->last_direction = snake->direction;

                if (i == snake_id && block_exists(x, y)) {
                    if (is_server) sendall_loss(0);
                    else send_loss();
                    delete_snake(snake_id);
                    continue;
                }

                if (x == apple_x && y == apple_y) {
                    snake->length++;
                    snake->coordinates = realloc(snake->coordinates, snake->length * 2 * sizeof(uint16_t));

                    snake->coordinates[(snake->length - 1)*2] = x;
                    snake->coordinates[(snake->length - 1)*2 + 1] = y;

                    if (is_server) {
                        generate_apple();
                        sendall_apple();
                    }

                } else {
                    putchar(snake->coordinates[0] + 1, snake->coordinates[1] + 1, ' ');

                    for (int i = 0; i<snake->length-1; i++) {  // shift
                        snake->coordinates[i*2] = snake->coordinates[(i+1) * 2];
                        snake->coordinates[i*2 + 1] = snake->coordinates[(i+1) * 2 + 1];
                    }
                    snake->coordinates[(snake->length - 1)*2] = x;
                    snake->coordinates[(snake->length - 1)*2 + 1] = y;
                }

                int color = i == snake_id ? COLOR_PAIR(COLOR_YOU) : COLOR_PAIR(COLOR_OPPONENT);
                attron(color);
                putchar(x + 1, y + 1, '#');
                attroff(color);
            }
        }
        refresh();
        gettimeofday(&last_render_time, 0);
        msleep(delay);
    }
}

int get_direction_by_key(int key) {
    switch (key) {
        case 's':
        case KEY_DOWN:
            return DOWN;
        case 'a':
        case KEY_LEFT:
            return LEFT;
        case 'w':
        case KEY_UP:
            return UP;
        case 'd':
        case KEY_RIGHT:
            return RIGHT;
    }
}

void keyboard_handler() {
    int key;
    int8_t direction;
    while (PLAYING) {
        key = getch();
        direction = get_direction_by_key(key);

        if (direction != snakes[snake_id].last_direction &&
            direction != (snakes[snake_id].last_direction + 2) % 4) {  // opposite
            snakes[snake_id].direction = direction;

            if (is_server) {  // server
                struct DirectionPktServer packet;
                packet.id = 0;
                packet.direction = direction;
                sendall_direction_pkt(packet);
            } else {  // client
                struct DirectionPkt packet;
                packet.magic = DIRECTION;
                packet.direction = direction;
                send(sd, &packet, sizeof(struct DirectionPkt), 0);
            }
        }
    }
}

void init_map() {
    putchar(0, 0, '+');
    putchar(w-1, 0, '+');
    putchar(0, h-1, '+');
    putchar(w-1, h-1, '+');
    for (int i = 1; i<w-1; i++) {
        putchar(i, 0, '-');
        putchar(i, h-1, '-');
    }
    for (int i = 1; i<h-1; i++) {
        putchar(0, i, '|');
        putchar(w-1, i, '|');
    }
    refresh();
    w -= 2;
    h -= 2; // borders
}

int8_t get_alives() {
    int8_t count = 0;
    for (int i = 0; i<MAX_SNAKE_COUNT; i++)
        if (snakes[i].live) count++;
    return count;
}

void send_snakes(int cd) {
    struct Snake* snake;
    for (int i = 0; i<MAX_SNAKE_COUNT; i++) {
        snake = snakes + i;
        if (snake->live) {
            send(cd, &snake->id, 1, 0);  // id
            send(cd, &snake->length, 2, 0);  // length
            send(cd, &snake->direction, 1, 0);  // direction
            send(cd, snake->coordinates, snakes->length * 2 * sizeof(uint16_t), 0);  // send coordinates
        }
    }
}

void set_snake_direction(uint16_t id, uint8_t direction) {
    snakes[id].direction = direction;
}

float timedifference_msec(struct timeval t0, struct timeval t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}

void sendall_connection_pkt(int cd, int id, int x, int y) {  // server
    struct ConnectionPkt pkt;
    pkt.magic = CONNECTION;
    pkt.id = id;
    pkt.x = x;
    pkt.y = y;

    sendall_pkt(&pkt, sizeof(struct ConnectionPkt));
}

void accept_connection(int cd) {  // server
    struct Snake* new_snake = generate_snake();
    new_snake->sd = cd;
    show_snake(new_snake);
    uint16_t new_x = new_snake->coordinates[0];
    uint16_t new_y = new_snake->coordinates[1];

    struct timeval current_time;
    gettimeofday(&current_time, 0);
    int sync = timedifference_msec(last_render_time, current_time);

    struct InitPkt pkt;
    pkt.new_snake_id = new_snake->id;
    pkt.alive_snakes = get_alives();
    pkt.new_x = new_x;
    pkt.new_y = new_y;
    pkt.apple_x = apple_x;
    pkt.apple_y = apple_y;
    pkt.delay = delay;
    pkt.sync = sync;
    send(cd, &pkt, sizeof(struct InitPkt), 0);

    send_snakes(cd);
    sendall_connection_pkt(cd, new_snake->id, new_x, new_y);
}

void server_handler(struct sockaddr_in* addr) {
    int sd, cd, maxfd;
    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) quit(1);
    if (bind(sd, addr, sizeof(*addr)) < 0) quit(1);
    listen(sd, 1);

    struct pollfd fds[MAX_SNAKE_COUNT+3] = {0};  // 3 = stdin + stdout + stderr
    fds[sd].fd = sd;
    fds[sd].events = POLLIN;
    maxfd = sd;

    while (PLAYING) {
        int nready = poll(fds, maxfd + 1, -1);

        if (fds[sd].revents & POLLIN) {
            cd = accept(sd, NULL, NULL);
            fds[cd].fd = cd;
            fds[cd].events = POLLIN;
            if (cd > maxfd) maxfd = cd;

            accept_connection(cd);

            if (--nready == 0) continue;
        }

        for (int i = 0; i <= maxfd; ++i) {
            if (fds[i].revents & POLLIN) {
                uint8_t magic;
                int ret = recv(i, &magic, 1, 0);

                int id = id_by_sd(i);

                if(ret == 0) {  // connection closed
                    fds[i].fd = -1;
                    fds[i].events = 0;
                    delete_snake(id);
                    snakes[id].online = FALSE;
                    close(i);
                    continue;
                }

                switch (magic) {
                    case DIRECTION:
                        uint8_t direction;
                        recv(i, &direction, 1, 0);
                        set_snake_direction(id, direction);
                        struct DirectionPktServer packet;
                        packet.id = id;
                        packet.direction = direction;
                        sendall_direction_pkt(packet);
                        break;
                    case LOSS:
                        delete_snake(id);
                        sendall_loss(id);
                        break;
                    case QUIT:
                        fds[i].fd = -1;
                        fds[i].events = 0;
                        delete_snake(id);
                        snakes[id].online = FALSE;
                        close(i);
                        sendall_quit(id);
                        break;
                }
            }
        }
    }
}

void recv_snakes(int sd, int alives) {
    uint8_t id;

    for (int i = 0; i<alives; i++) {
        recv(sd, &id, 1, 0);
        struct Snake* snake = snakes + id;
        snake->live = TRUE;
        snake->id = id;
        recv(sd, &snake->length, 2, 0);
        recv(sd, &snake->direction, 1, 0);
        int coordinates_size = snake->length * 2 * sizeof(uint16_t);
        snake->coordinates = malloc(coordinates_size);
        recv(sd, snake->coordinates, coordinates_size, 0);
        show_snake(snake);
    }
}

void client_connect(struct sockaddr_in addr) {
    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sd, &addr, sizeof(struct sockaddr_in)) < 0) quit(0);

    struct InitPkt pkt;
    recv(sd, &pkt, sizeof(struct InitPkt), 0);
    snake_id = pkt.new_snake_id;
    generate_my_snake(pkt.new_x, pkt.new_y);
    apple_x = pkt.apple_x; apple_y = pkt.apple_y;
    show_apple();
    delay = pkt.delay;

    recv_snakes(sd, pkt.alive_snakes);

    msleep((int)(pkt.delay - pkt.sync));  // sync render
}

void client_handler() {
    while (PLAYING) {
        uint8_t magic, id;
        if (!recv(sd, &magic, 1, 0)) quit(1);  // connection closed
        switch (magic) {
            case DIRECTION:
                uint8_t direction;
                recv(sd, &id, 1, 0);
                recv(sd, &direction, 1, 0);
                set_snake_direction(id, direction);
                break;
            case APPLE:
                recv(sd, &apple_x, 2, 0); recv(sd, &apple_y, 2, 0);
                show_apple();
                break;
            case CONNECTION:
                uint16_t x, y;
                recv(sd, &id, 1, 0);
                recv(sd, &x, 2, 0); recv(sd, &y, 2, 0);
                generate_snake_by_coordinates(id, x, y);
                break;
            case LOSS:
            case QUIT:
                recv(sd, &id, 1, 0);
                delete_snake(id);
                break;
        }
    }
}

void init_colors() {
    if (!has_colors()) {
        endwin();
        puts("Your terminal does not support color");
        exit(1);
    }
    start_color();
    init_pair(COLOR_YOU, COLOR_RED, COLOR_BLACK);
    init_pair(COLOR_OPPONENT, COLOR_BLUE, COLOR_BLACK);
    init_pair(COLOR_APPLE, COLOR_GREEN, COLOR_BLACK);
}

void init() {
    srand(time(NULL));
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    signal(SIGINT, quit);
    init_colors();
    w = 100; h = 40;
    init_map();
}

int main(int argc, char** argv) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(4444);
    int opt;
    while ((opt = getopt(argc, argv, "lcp:h:d:")) != -1) {
        switch (opt) {
            case 'l':
                is_server = TRUE;
                break;
            case 'c':
                is_server = FALSE;
                break;
            case 'p':
                addr.sin_port = atoi(optarg);
                break;
            case 'h':
                addr.sin_addr.s_addr = inet_addr(optarg);
                break;
            case 'd':
                delay = atoi(optarg);
                break;
        }
    }

    pthread_t net_thread;
    switch (is_server) {
        case TRUE:  // server
            init();
            snake_id = 0;
            generate_my_snake(START_X, 0);
            generate_apple();
            pthread_create(&net_thread, NULL, server_handler, &addr);
            break;
        case FALSE:  // client
            init();
            client_connect(addr);
            pthread_create(&net_thread, NULL, client_handler, NULL);
            break;
        case -1:
            printf("Error\n");
            return 1;
    }

    pthread_t keyboard_thread;
    pthread_create(&keyboard_thread, NULL, keyboard_handler, NULL);
    render();
    return 0;
}
