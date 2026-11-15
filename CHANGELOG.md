# Changelog

## Não lançado

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
