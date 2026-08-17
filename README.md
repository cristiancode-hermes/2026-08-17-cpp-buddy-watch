# buddy-watch — Monitor de fragmentación de memoria Linux (C++23)

`buddy-watch` es un monitor de **fragmentación de memoria** escrito en **C++23 sin dependencias externas**. Lee `/proc/buddyinfo` del kernel y analiza la distribución de bloques libres por orden en el *buddy allocator*: memoria libre por zona, bloque contiguo máximo, disponibilidad de bloques de 2 MiB para *Transparent Huge Pages* (THP) y un índice de fragmentación externa — con modo watch con deltas, salida JSON para scripting y 26 auto-checks.

## Características

- **Parseo de `/proc/buddyinfo`**: las 11 órdenes (0..10) de bloques libres por nodo y zona (DMA, DMA32, Normal…), tolerante a líneas malformadas o con números de más/menos.
- **Índice de fragmentación**: `frag% = 100 × (1 − páginas en bloques ≥ orden 4 / total libre)` — 0% = memoria en bloques grandes, 100% = solo bloques de 4 KiB sueltos.
- **THP readiness**: conteo de bloques de **orden 9 (2 MiB)** — los bloques contiguos que THP necesita para asignar páginas grandes.
- **Bloque contiguo máximo** por zona y global (hasta 4 MiB en orden 10).
- **Tabla de histograma**: los conteos de bloques por orden tal cual los reporta el kernel, con cabecera de órdenes.
- **Modo watch** (`--watch N`): refresco periódico con delta de memoria libre, mínimo histórico y fragmentación máxima; `--timeout T` para salida automática (ideal para cron/scripts).
- **Salida JSON** (`--json`): snapshot completo con zonas, bloques, totales y métricas — validada con `python3 -m json.tool`.
- **`--demo`**: snapshot sintético realista para contenedores/VMs donde `/proc/buddyinfo` no esté disponible.
- **`--no-color`**: paleta ANSI mutable que se vacía por completo (verificable con `grep -c $'\x1b'` → 0).
- **`--selftest`**: 26 checks sobre parseo, consistencia aritmética, métricas, JSON y unidades.
- **Cero dependencias**: compilación directa con `g++ -std=c++23`.

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o buddy-watch
```

## Uso

```bash
./buddy-watch                        # snapshot completo con tabla e histograma
./buddy-watch --no-histogram         # solo la tabla de resumen por zona
./buddy-watch --json                 # snapshot JSON (scripting)
./buddy-watch --watch 5              # refresco cada 5 s con deltas
./buddy-watch --watch 5 --timeout 60 # 60 s de monitoreo y salida
./buddy-watch --demo                 # datos sintéticos
./buddy-watch --selftest             # 26 auto-checks
./buddy-watch --no-color             # sin ANSI (pipes/logs)
```

### Ejemplo de salida

```
$ ./buddy-watch
buddy-watch — fragmentación de memoria Linux (/proc/buddyinfo)
snapshot 2026-08-17 05:11:04 · página 4 KiB

zone    free        max contig  frag    THP 2MiB
--------------------------------------------------
DMA     13.00 MiB   4.00 MiB      0.0%  2
DMA32   513.87 MiB  2.00 MiB     30.4%  2
Normal  1.33 GiB    2.00 MiB     38.7%  3
--------------------------------------------------
TOTAL  1.85 GiB libres · frag 36.2% · contig máx 4.00 MiB · 7 bloques THP (2 MiB)

bloques libres por orden (4 KiB · 2^orden)
zone    o0    o1    o2    o3    o4    o5    o6    o7    o8    o9    o10
DMA     0     0     0     0     0     0     0     0     1     2     2
DMA32   5330  2778  1752  2769  1547  894   352   88    13    2     0
Normal  24776 12172 8447  6509  4473  2048  731   199   12    3     0
```

### JSON

```bash
$ ./buddy-watch --json
{
  "ts": 1787000000,
  "total_free_pages": 494381,
  "total_free_kib": 1977524,
  "fragmentation_pct": 36.2,
  "thp_blocks": 7,
  "max_contig_order": 10,
  "zones": [
    {"node": 0, "zone": "DMA", "free_pages": 3328, "blocks": [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2]},
    ...
  ]
}
```

## Interpretación de métricas

| Métrica | Significado |
|---|---|
| `free` | Páginas libres por zona (4 KiB cada una) |
| `max contig` | Mayor bloque contiguo disponible (2^orden × 4 KiB) |
| `frag` | Fragmentación externa: % de memoria libre que vive en bloques pequeños (< 64 KiB) |
| `THP 2MiB` | Bloques de orden 9 disponibles — cada uno sirve para una página grande de 2 MiB |

Un `frag` alto con memoria libre abundante indica que la memoria está repartida en bloques pequeños: la asignación de páginas grandes (THP) o buffers contiguos fallará aunque haya RAM libre.

## Estructura

```
2026-08-17-cpp-buddy-watch/
├── src/main.cpp        # C++23 stdlib-only (~560 líneas)
├── buddy-watch         # binario compilado
├── capture.mjs         # captura de screenshots (Puppeteer)
├── screenshot.html     # terminal sintética para la captura
└── README.md
```

## Detalles técnicos

- **Formato real de `/proc/buddyinfo`**: `Node 0, zone      DMA      0  0  0 ...` — el parser consume `Node`/id/`zone` por tokens (tolerando la coma del id y líneas sin la palabra `zone`) y lee hasta 11 enteros.
- **Aritmética verificada**: cada zona = Σ `bloques[o] × 2^o` páginas; el total global es la suma de zonas (checks 8-10 del selftest).
- **`--no-color` real**: la paleta ANSI es mutable (no `constexpr`) y se vacía al parsear el flag — `./buddy-watch --no-color | grep -c $'\x1b'` da 0.
- **Watch sin pérdida de salida**: `std::cout << std::flush` al final de cada iteración — al matar el proceso con timeout externo el log conserva todos los renders.
