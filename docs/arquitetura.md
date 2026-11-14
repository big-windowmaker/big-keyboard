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
3. A janela nunca pega o foco: layer-shell `keyboard-interactivity none` no
   Wayland; tipo DOCK sem foco no X11. Reservar espaço: zona exclusiva do
   layer-shell / `_NET_WM_STRUT_PARTIAL`.
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

## Segurança

- Roda como o usuário; nenhuma regra udev própria. O `/dev/uinput` já é do
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
