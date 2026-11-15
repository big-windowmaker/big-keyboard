# Arquitetura

## Escolhas

- **C + GTK 3**, como o resto do Big WindowMaker (tabbed-dock, tela de
  bloqueio). GTK 3 dá layer-shell no Wayland (gtk-layer-shell), toques com
  sequência por dedo e o tema OneStepBack da sessão nas configurações; o
  teclado e o controle na tela são desenhados com cairo, para ficarem
  idênticos ao teclado da tela de bloqueio e baratos de redesenhar.
- **evdev + libudev, não SDL3**, para o controle físico: integra ao laço do
  GLib por descritores (sem thread nem polling), é o mesmo núcleo da tela de
  bloqueio, e os drivers do kernel (xpad, hid-playstation, hid-steam,
  8BitDo) já seguem a API de gamepad do kernel. Joysticks genéricos antigos
  (BTN_TRIGGER...) são mapeados pela ordem dos botões.
- **uinput** para o controle virtual: o kernel cria um gamepad que SDL,
  Steam/Proton, Wine e navegadores enxergam; some quando o descritor fecha,
  então nada fica órfão nem preso numa queda.

## Módulos

| Diretório | Papel |
|---|---|
| `src/core/bk-keys.c` | layouts, níveis, modificadores, teclas mortas → eventos de saída (texto ou keysym+mods) |
| `src/core/bk-geom.c` | retângulos das teclas; toque; modo apontar dos analógicos |
| `src/core/bk-config.c` | preferências versionadas, validadas, gravação atômica |
| `src/input/big-wm-gamepad.c` | controles físicos (udev, evdev, conexão a quente, repetição, navegação espacial) |
| `src/input/bk-rtouch.c` | toque remoto: o touchscreen virtual do Sunshine (protocolo B, SYN_DROPPED, udev) |
| `src/input/bk-inject.c` | entrega das teclas: virtual-keyboard (Wayland) e XTest (X11) |
| `src/gamepad/bk-vpad.c` | controle virtual uinput |
| `src/gamepad/bk-controls.c` | controle na tela: layout, toques, gravação |
| `src/ui/` | aplicativo (ações D-Bus), teclado, sobreposição/editor, configurações |
| `src/ui/bk-focus.c` | campo de texto em foco pela acessibilidade (AT-SPI): abrir o teclado como no celular |
| `src/dockapp/` | dockapp X11 (Window Maker; wlmaker pelo XWayland) |

O núcleo (`core`, `input/big-wm-gamepad`, `gamepad`) não depende de X11 nem
Wayland e tem testes próprios (`tests/`).

## Fluxo

1. O aplicativo fica residente (`--daemon`, autostart da sessão) só com
   descritores parados: controles (udev + evdev) e o arquivo do tema.
2. Um clique, toque ou botão vira um `BkKey`; `bk_state_press` devolve os
   eventos de saída; `bk_inject_out` os entrega:
   - **Wayland**: um keymap XKB com um keycode por keysym usado (gerado sob
     demanda), modificadores pelo pedido `modifiers`. Qualquer caractere
     Unicode (emojis, acentos) sem depender do layout da sessão; o wlmaker
     mantém o keymap do teclado virtual.
   - **X11**: XTest com a tecla do layout da sessão (Shift se preciso); um
     caractere fora do layout usa um keycode livre remapeado (rodízio de 4,
     devolvidos ao sair).
3. A janela nunca pega o foco (veja [Janela do teclado](#janela-do-teclado)).
4. O dockapp fala com o aplicativo pelas ações `org.gtk.Actions`
   (`toggle`, `settings`, `gamepad`, `hybrid`, `release`, `quit`) e segue os
   estados `mode` e `pad-connected`. O Big Remote Play usa `set-gamepad`,
   `remote-touch`, `overlay-output`, `release` e o estado
   `remote-touch-status`.

## Toque remoto (celular → Moonlight → Sunshine)

```text
celular (dedos) → Moonlight (LiSendTouchEvent) → Sunshine (libvirtualhid)
  → touchscreen uinput "libvirtualhid Touchscreen" (0..19200 × 0..10800,
    normalizado à imagem do monitor transmitido)
  → bk-rtouch (leitura direta, EVIOCGRAB no modo exclusivo)
  → bk_ctl_touch (um toque por slot) → bk-vpad (gamepad uinput) → jogo
```

- **Sem o compositor**: o wlmaker não entrega toques (o dispositivo de
  toque é ignorado); no X11/KDE o toque viraria clique fora dos controles.
  Ler o nó direto funciona igual em todos. Só enquanto o controle está
  visível; fechado, o dispositivo volta ao desktop.
- **Coordenadas**: o Moonlight normaliza o toque à área do vídeo (sem as
  barras pretas do celular); o Sunshine põe isso nos eixos do dispositivo;
  aqui `px = x / 19200 × largura do monitor`. Vale para qualquer proporção,
  rotação ou escala fracionária, desde que o controle esteja no monitor
  transmitido (`overlay-output`).
- **Nada preso**: o dispositivo sumir solta tudo; `SYN_DROPPED` relê o
  estado (`EVIOCGMTSLOTS`). O Sunshine 2026.914 mantém o touchscreen depois
  que o cliente sai e não cancela os contatos ao desconectar; por isso o
  Big Remote Play chama `release` quando a sessão cai, e um dedo solto à
  força só volta a valer se o celular voltar a movê-lo.
- **Vários Sunshine** (compartilhamento normal e a tela privada do Game
  Window): todos os touchscreens remotos presentes são lidos.
   estados `mode` e `pad-connected`: o aplicativo é a única fonte do estado
   (fechar uma janela por fora também muda o desenho).

## Dockapp

Dois botões num tile de 64×64: moldura de 1 px, sulco de 2 px entre as
metades, que, como a moldura, não aciona nenhum botão. `src/dockapp/bk-dock.c` não
depende de X11 (geometria, áreas de clique, clique × duplo clique, desenho
cairo) e é testado em `tests/test-dock.c`, inclusive por pixels (moldura,
tamanho dos símbolos, contraste WCAG, estados); `bk-dockapp.c` é a cola.

- **Clique × duplo clique**: um clique só vale depois do intervalo de duplo
  clique, contado de um aperto ao outro (`DoubleClickTime` do Window Maker,
  que o big-wm-wprefs-sync leva ao wlmaker; 250 ms sem a chave). O segundo
  aperto na mesma metade, perto, vira duplo clique: só as configurações,
  nenhuma alternância. Um aperto na outra metade dispara na hora o clique
  pendente. Segurar além do intervalo alterna ao soltar; soltar fora do
  botão ou arrastar (o Window Maker move o tile) não alterna. O único
  temporizador existe enquanto há um clique pendente.
- **Estados**: normal, hover, apertado (a tecla afunda), ativo (janela
  aberta: afundado e mais claro) e desabilitado (o controle na tela sem
  acesso ao `/dev/uinput`; a dica explica, e o clique só fecha).
- **Dica**: uma janela `tooltip` do GTK, posicionada pelo ponteiro (no
  XWayland, a janela do dockapp fica em 0,0 na raiz X).
- **LEDs**: embaixo de cada botão, verde com a janela aberta, cinza fechada
  (mais fraco quando o botão está desabilitado). O símbolo sobe para dar
  lugar ao LED (`bk_dock_led_rect`).
- **Menu Principal**: o `big-wm-appmenu` do big-wm o lista em Dockapps >
  Ferramentas quando o pacote está instalado.

## Janela do teclado

| Modo | X11 | Wayland (layer-shell) |
|---|---|---|
| Flutuante | janela UTILITY com a moldura do gerenciador, `accept-focus` falso (WM_HINTS input=False); o tamanho vem do gerenciador e as teclas acompanham (dicas de proporção) | moldura desenhada (título com fechar, barra de redimensionar), âncora em cima à esquerda, margens = posição, zona exclusiva -1 |
| Encaixado | tipo DOCK, largura do monitor, strut embaixo | âncoras embaixo e nas laterais, zona exclusiva = altura (reservar) |
| Por cima de tela cheia | override-redirect com a moldura desenhada; volta ao topo quando `_NET_CLIENT_LIST_STACKING` ou `_NET_ACTIVE_WINDOW` mudam na raiz | camada OVERLAY em vez de TOP |

- **Por que a moldura desenhada no Wayland**: uma janela xdg comum ganharia
  o foco do teclado ao ser tocada, e o `zwp_virtual_keyboard_v1` entrega as
  teclas a quem tem o foco: elas iriam para o próprio teclado.
- **Medidas**: tudo sai da unidade `u` (largura de uma tecla comum). A
  largura escolhida (ou a do tamanho das teclas) dá `u`; a altura vem da
  proporção; o que sobra do arredondamento vai para a coluna. Encaixado:
  `u` limitado a 42% da altura do monitor, e as teclas se esticam na
  horizontal para preencher a largura.
- **Mover no layer-shell**: o dedo só é conhecido em relação à própria
  superfície, que anda debaixo dele. Cada posição nova só é pedida depois
  que o compositor confirmou a anterior (`wl_display_sync` após o quadro),
  e o dedo é lido em relação à posição já aplicada. O wlmaker antes do
  patch 0152 parava de entregar o ponteiro quando ele saía da superfície
  com o botão apertado; a 0152 dá aos painéis comuns a captura implícita
  que as janelas têm em outros compositores.
- **Lugar e tamanho**: `FloatX`, `FloatY` e `FloatWidth` em `[Keyboard]`,
  gravados 0,7 s depois do último movimento. Mudar o tamanho das teclas
  volta à largura padrão; "Restaurar o lugar e o tamanho" zera os três.
- **Trocar de modo ou de "por cima"** refaz a janela (o layer-shell só é
  configurado antes de mapear, e override-redirect é do tipo da janela), no
  mesmo estado de visível ou oculta.
- **Campo de texto (AT-SPI)**: `object:state-changed:focused` num objeto
  editável e não somente leitura (ou um terminal) abre o teclado e o marca
  como aberto sozinho; sair do campo fecha depois de 400 ms (passar de um
  campo a outro tira e põe o foco), só se ele abriu sozinho. Ao ligar,
  pede `org.a11y.Status.IsEnabled` (Firefox, Chromium e Qt só expõem a
  acessibilidade assim). Antes de iniciar a libatspi, testa a conexão com o
  barramento de acessibilidade: sem ele a biblioteca encerraria o processo.

## Controle na tela no X11

Ele é uma janela override-redirect de tela inteira com entrada só nos
botões. Como janela DOCK "sempre acima" ficava atrás do jogo em tela cheia
(Window Maker, KWin e xfwm4 põem a tela cheia acima das docas, como manda o
EWMH); fora do gerenciador, ela volta ao topo quando
`_NET_CLIENT_LIST_STACKING` ou `_NET_ACTIVE_WINDOW` mudam na raiz. Sem
moldura, os toques fora dos botões vão direto para a janela de baixo. No
Wayland, a camada OVERLAY fica acima da tela cheia; o wlmaker só passou a
respeitar isso no pacote 38 (patch 0151 em big-windowmaker/packages).

## Segurança

- Roda como o usuário. Única regra udev própria
  (`data/70-big-keyboard-remote-touch.rules`): `uaccess` só para o nó
  virtual chamado exatamente `libvirtualhid Touchscreen` (o touchscreen do
  Sunshine; a regra do Sunshine não o cobre). O `/dev/uinput` já é do
  usuário do assento ativo (`uaccess`) com o `steam-devices` ou o
  `game-devices-udev`; sem ele, só o controle virtual fica indisponível.
- O leitor de controle aceita só dispositivos que o udev marca como joystick
  e recusa os que têm teclas de letras: nunca lê um teclado.
- **Tela de bloqueio separada**: o teclado de uso geral entrega teclas aos
  aplicativos da sessão; o da tela de bloqueio é parte do bloqueador
  (módulo do gtklock, `auth_big_wm` do xsecurelock, no big-wm), escreve só
  no campo de senha dele e nunca passa por este programa. Com a tela
  bloqueada (ext-session-lock), um cliente comum não recebe foco nem pode
  digitar na tela de bloqueio.
- Nenhuma senha, token ou cookie é guardado; nada sensível vai para log.
- Transições de modo, fechar o teclado, fechar o controle na tela e
  `--release` soltam modificadores, teclas e botões.

## Xbox Cloud Gaming

O Better xCloud (MIT) já converte teclado e mouse em controle no navegador
(Gamepad API, Pointer Lock) e traz controles de toque. Duas emulações juntas
duplicam entradas, então o Big WM Virtual Keyboard escolhe uma: **Better
xCloud no navegador** (padrão; a seção de configurações mostra os navegadores
instalados, as instruções das fontes oficiais e os avisos) ou **controle
virtual do Linux** (toque na tela; o navegador vê o gamepad uinput). Estado
em novembro de 2026: o Better xCloud funciona em `xbox.com/play`, não em
`play.xbox.com` (prévia), segundo o próprio projeto.
