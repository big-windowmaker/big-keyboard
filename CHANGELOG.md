# Changelog

## 0.3.0 (2026-11-27)

- **Interface em inglês e português**: o texto da interface passa pelo
  gettext, com o inglês como texto-fonte e a tradução pt_BR completa (241
  mensagens). Com `LANG=pt_BR.UTF-8` nada muda (as 9 páginas das
  Configurações conferidas pixel a pixel); em outro idioma, a interface sai
  em inglês. O nome do layout padrão do controle (`padrão`) continua o mesmo
  em todos os idiomas, porque é também o nome do arquivo.

## 0.2.4 (2026-11-17)

- **Teclado e controle ocultos: touchpad**. No modo de toque remoto
  `mouse`, com os dois ocultos, a tela do celular agora é um touchpad:
  arrastar o dedo move o ponteiro de onde ele está, um toque rápido é o
  clique esquerdo, segurar meio segundo e arrastar arrasta, um toque com
  outro dedo é o clique direito e dois dedos rolam. No 0.2.3 era toque
  direto (o ponteiro pulava para o dedo), que continua com *Sempre toque
  direto*.

## 0.2.3 (2026-11-17)

- **Mouse do celular com o teclado e o controle ocultos**: novo modo de
  toque remoto `mouse` (`--remote-touch=mouse`, ação `remote-touch`,
  Configurações > Celular > *Também o mouse com os dois ocultos*). É o
  `exclusive` que continua lendo o touchscreen do Sunshine com tudo
  oculto, e então todo dedo vira o mouse em toque direto (ou touchpad, com
  *Sempre touchpad*). Com o teclado ou o controle abertos, nada muda. O Big
  Remote Play o pede onde o desktop ignora o toque do Sunshine (wlmaker):
  antes, ocultar os dois parava o ponteiro do celular. Sem um ponteiro
  virtual para mover, o celular não é agarrado com tudo oculto (os dedos
  iriam fora). As regras ficaram
  em `bk_tm_reads`, `bk_tm_exclusive` e `bk_tm_mode_for`
  (`src/input/bk-touchmouse.c`), testadas em `tests/test-touchmouse.c`.

## 0.2.2 (2026-11-15)

- **Controle na tela visível na tela de bloqueio** (Wayland): com a sessão
  bloqueada, o controle continuava funcionando pelo Moonlight, mas o
  desenho ficava atrás do bloqueio (ext-session-lock). Enquanto o controle
  está na tela, o layout, o monitor e o PID ficam em
  `$XDG_RUNTIME_DIR/big-keyboard/lock-gamepad.ini`, e o módulo do gtklock
  do big-wm (`big-wm-lockscreen`) o desenha por cima do bloqueio, só como
  imagem: os toques continuam vindo para cá. O desenho foi para
  `src/gamepad/bk-ctlpaint.{c,h}` (só cairo, Pango e GLib), copiado pelo
  `sync.sh` do big-wm.
- **Botão Guia no controle na tela** (o do logo: Xbox, PS, Home): no alto
  ao centro, logo abaixo do botão do teclado e entre Select e Start, em
  todos os modelos. Desenhado em vetor conforme a família dos botões (esfera
  com X, "PS" ou a casinha do Home), com um halo na cor de destaque. Os
  layouts já salvos o ganham ao abrir (desligado, para ligar e mover no
  editor, se algo do layout estiver no lugar dele).

## 0.2.1 (2026-11-15)

- **Celular como mouse e teclado**: o touchscreen do Sunshine é lido com o
  teclado aberto também (antes, só com o controle na tela). Cada dedo vai
  para o que está embaixo dele: as teclas (a barra de título move a
  janela), os controles, ou o mouse. Só o teclado: touchpad (arrastar move,
  toque é o clique esquerdo, parar meio segundo e mover arrasta). Teclado e
  controle juntos: toque direto (o ponteiro vai até o dedo e clica). Nos
  dois, um dedo segurado e o toque de outro é o clique direito, e dois
  dedos rolam. Ponteiro pelo `zwlr_virtual_pointer_v1` (Wayland, preso ao
  monitor transmitido) ou XTest (X11). Com um celular conectado, o teclado
  abre no monitor transmitido.
- **Configurações > Celular**: modo do toque, mouse pelo toque
  (automático, touchpad, toque direto ou desligado), velocidade do
  touchpad e o estado em palavras.
- **Estilos e modelos do controle na tela**: cinco estilos (clássico,
  moderno, retrô, neon, minimalista), botões de Xbox, PlayStation ou
  Nintendo (nomes e cores), cor de destaque, analógico fixo ou flutuante,
  direcional em cruz, quatro setas ou disco, e animação de toque (afundar,
  brilho, onda, encolher), gravados com o layout. O pino do analógico segue
  o dedo e o direcional acende o lado apertado. Sete modelos prontos (Xbox,
  PlayStation, Nintendo Switch, retrô, arcade, corrida, mínimo) e uma
  prévia nas configurações. Layouts antigos continuam como eram.
- **Correções**: o layout escolhido em Configurações > Teclado vale na
  hora (antes, só depois de reiniciar a sessão); o controle físico chega
  aos botões Controle, Configurar e Ocultar; os emojis do painel ☺ pedem
  uma fonte de emoji (`noto-fonts-emoji`, nova dependência), sem a qual
  apareciam como quadrados com o código.
- **Ícone novo**: controle sobre teclado, em branco, num quadrado
  arredondado verde-azulado com gradiente (o estilo dos ícones do Big
  Remote Play Together). `librsvg` passa a ser dependência explícita.
- **Dockapp**: 58×58 no ladrilho de 64×64, como os dockapps clássicos,
  deixando livre uma borda fina (3 px) por onde segurar e arrastar o
  ladrilho para outro lugar. No wlmaker não havia como arrastá-lo: ele só
  move um dockapp X11 pela borda do ladrilho (ou com o modificador de mover
  janelas), e a janela de 64×64 a cobria inteira.
- **Teclado flutuante acima do Dock com Abas**: sem posição gravada, o
  lugar padrão é calculado de novo a cada vez que o teclado aparece. Antes,
  ficava preso à área útil do início da sessão, quando o Dock com Abas
  ainda não tinha reservado a borda de baixo, e a fileira da barra de
  espaço ficava atrás dele (X11). No Wayland, o GTK 3 dá o monitor inteiro
  como área útil; a altura que sobra depois dos painéis layer-shell vem
  agora do compositor (uma superfície sem buffer, que nunca aparece).

## 0.2.0 (2026-11-15)

- **Janela do teclado**: flutuante (padrão), com a moldura do Window Maker
  (título, fechar, barra de redimensionar), vai para qualquer lugar e muda
  de tamanho com as teclas acompanhando, e lembra o lugar e o tamanho; ou
  encaixado embaixo, na largura inteira, como no celular. No Wayland a
  moldura é desenhada pelo teclado (uma janela comum roubaria o foco).
- **Coluna à direita das teclas**: botões grandes, um embaixo do outro,
  para o controle na tela, as configurações e (encaixado) ocultar. Saem os
  botões pequenos da barra de título.
- **Abrir ao tocar num campo de texto** (AT-SPI), e fechar ao sair dele se
  abriu sozinho. Sem o barramento de acessibilidade, a opção só não age.
- **Ficar por cima**: como as outras janelas, por cima das janelas ou por
  cima de tudo, até de jogos, vídeos e apresentações em tela cheia.
- **Controle na tela acima da tela cheia no X11**: janela override-redirect
  que volta ao topo quando o gerenciador reempilha (antes, DOCK atrás da
  tela cheia). No wlmaker, pede o pacote 38.
- **Dockapp**: LED embaixo de cada botão (verde aberto, cinza fechado); o
  dockapp de dois botões e o toque remoto agora estão no mesmo ramo.
- Arrastar o teclado no wlmaker pede o pacote 39 (o ponteiro se perdia ao
  sair da superfície).
- **Trocar entre teclado e controle na tela**: a barra de título do teclado
  ganhou um botão com um controle, e o controle na tela, um botão redondo
  com um teclado (no alto, ao centro, fora do layout e sem cobrir nenhum
  controle do layout padrão). A troca acontece ao tirar o dedo, também
  pelo celular; o gamepad virtual continua ligado (o jogo não o vê sair) e
  o teclado abre no monitor do controle. Fechar o teclado depois da troca
  desliga o gamepad.
- **Toque remoto (celular pelo Moonlight)**: com o controle na tela aberto,
  os dedos que chegam pelo touchscreen virtual do Sunshine
  (`libvirtualhid Touchscreen`) são lidos direto do kernel, cada um como um
  toque independente — direcional + botões, dois analógicos, gatilhos, cinco
  dedos. Não depende do compositor (o wlmaker não entrega toques; no
  X11/KDE eles virariam cliques). Modo `exclusive` (padrão: só o controle
  recebe os toques), `shared` ou `off` (`--remote-touch`, ação
  `remote-touch`).
- **Monitor do controle na tela** (`--output=DP-1`, ação `overlay-output`):
  o controle fica no monitor transmitido; o nome do conector vem do
  xdg-output no Wayland e do RandR no X11.
- Ações D-Bus `set-gamepad` (b) e estado `remote-touch-status`, para o
  Big Remote Play.
- Jogar ↔ editar solta o que estava apertado; o editor também aceita o
  dedo remoto.
- Dockapp com dois botões de verdade: a tecla Enter ↵ abre/oculta o teclado
  e o direcional abre/oculta o controle na tela; duplo clique numa metade
  abre as configurações sem alternar nada; moldura de 1 px, símbolos
  maiores, estados de hover, aperto, ativo e desabilitado, dica por metade.
- Configurações > Geral > Modo de exibição: independente (padrão) ou
  exclusivo (abrir o teclado oculta o controle na tela e vice-versa).

## 0.1.0 — 2026-11-14

Primeira versão: teclado na tela (ABNT2, US, US internacional, painéis Fn e
símbolos/emojis, modificadores de um toque/travados, teclas mortas),
digitação no Wayland (virtual-keyboard) e no X11 (XTest) sem roubar o foco,
digitação pelo controle físico (cursor e modo apontar dos analógicos),
controle na tela de toque com controle virtual uinput e editor, dockapp
metade teclado, metade controle, configurações em seções e integração com o
Better xCloud. Testes em `tests/` e `docs/qa-2026-11-14.md`.
