# hpopi5b-panel —— 运维面板 + 自研锁屏（X11 + cairo + pango，无 GUI 工具包）
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = x11 xscrnsaver cairo cairo-xlib pangocairo
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

BIN  = hpopi5b-panel
AUTH = hpopi5b-auth
OBJ  = src/panel.o src/metrics.o src/remote.o src/theme.o src/view.o src/view_panel.o src/view_login.o src/auth.o \
       src/scenes.o src/scene_panel.o src/scene_trending.o src/scene_news.o

all: $(BIN) $(AUTH)

# 只构建面板本体（不含 setuid 的 PAM 认证助手）。
# 用途：纯显示终端（--kiosk，没有登录页）不需要任何特权代码，也就不该装 libpam-dev。
panel: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(AUTH): src/hpopi5b-auth.c
	$(CC) -O2 -Wall -Wextra -o $@ $< -lpam

src/%.o: src/%.c src/metrics.h src/theme.h src/view.h src/remote.h src/scene.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	install -m 755 $(BIN) /usr/local/bin/$(BIN)
	install -d -m 755 /usr/local/libexec
	install -m 4755 -o root -g root $(AUTH) /usr/local/libexec/$(AUTH)
	# 远程探针：被 ssh 喂到对方机器的 stdout（不是可执行文件，不需要 +x）
	install -d -m 755 /usr/local/share/hpopi5b-panel
	install -m 644 tools/probe-remote.pl /usr/local/share/hpopi5b-panel/probe-remote.pl

clean:
	rm -f $(OBJ) $(BIN) $(AUTH)

.PHONY: all panel clean install
