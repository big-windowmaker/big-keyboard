# Changelog

## Não lançado

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
