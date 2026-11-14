# Changelog

## Não lançado

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

## 0.1.0 — 2026-11-14

Primeira versão: teclado na tela (ABNT2, US, US internacional, painéis Fn e
símbolos/emojis, modificadores de um toque/travados, teclas mortas),
digitação no Wayland (virtual-keyboard) e no X11 (XTest) sem roubar o foco,
digitação pelo controle físico (cursor e modo apontar dos analógicos),
controle na tela de toque com controle virtual uinput e editor, dockapp
metade teclado, metade controle, configurações em seções e integração com o
Better xCloud. Testes em `tests/` e `docs/qa-2026-11-14.md`.
