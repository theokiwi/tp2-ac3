#!/usr/bin/env bash
# Refaz tudo do zero: compila, gera traces, roda sanidade, roda a bateria.
# Uso: bash sim/scripts/run_all.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SIM="$ROOT/sim"
SRC="$SIM/src"
BIN="$SIM/bin"
TRACES="$SIM/traces"
RESULTS="$SIM/results"

mkdir -p "$BIN" "$TRACES" "$RESULTS"

echo "== compilando =="
gcc -O2 -Wall -o "$BIN/gen_trace" "$SRC/gen_trace.c" -lm
gcc -O2 -Wall -o "$BIN/sim" "$SRC/sim.c"

# ---- parametros fixos do experimento ----
VM_BLOCKS=16384         # 16 GB / 1 MB por bloco
PRE_PERIODS=30
POST_ACCESSES=20000
TRACE_TOTAL=300000      # cobre PRE_PERIODS * periodo_denso + POST_ACCESSES
RAM_FRACTIONS="0.25 0.50 0.75"
PERIODS="sparso:200 medio:2000 denso:9000"
POLICIES="opt lru aging chunkq clock"
SEEDS="1001 1002 1003 1004 1005"

echo "== gerando traces =="
gen() { # workload vm_blocks total seed outfile
    if [ ! -f "$5" ]; then
        "$BIN/gen_trace" "$1" "$2" "$3" "$4" "$5"
    fi
}

gen sequential $VM_BLOCKS $TRACE_TOTAL 1 "$TRACES/sequential.bin"
for a in 0.8 0.99 1.2; do
    for s in $SEEDS; do
        gen "zipf:$a" $VM_BLOCKS $TRACE_TOTAL "$s" "$TRACES/zipf_${a}_${s}.bin"
    done
done
for s in $SEEDS; do gen uniform $VM_BLOCKS $TRACE_TOTAL "$s" "$TRACES/uniform_${s}.bin"; done
for s in $SEEDS; do gen phasechange $VM_BLOCKS $TRACE_TOTAL "$s" "$TRACES/phasechange_${s}.bin"; done
for s in $SEEDS; do gen bursty $VM_BLOCKS $TRACE_TOTAL "$s" "$TRACES/bursty_${s}.bin"; done

echo "== verificacoes de sanidade =="
SANITY_LOG="$RESULTS/sanity.txt"
: > "$SANITY_LOG"

check() { # descricao valor_obtido condicao_shell
    local desc="$1" val="$2" cond="$3"
    if eval "$cond"; then
        echo "PASS  $desc  (valor=$val)" | tee -a "$SANITY_LOG"
    else
        echo "FAIL  $desc  (valor=$val)" | tee -a "$SANITY_LOG"
    fi
}

# meta_prefix "t," aqui produz linhas: t,<policy>,<page_ins>,<split_precision>,<lru_agreement>,<decision_time_ms>,<post_accesses>,<eviction_count>
# ou seja: campo1=t campo2=policy campo3=page_ins campo4=split_precision campo5=lru_agreement

# 1) carga uniforme, RAM 50% -> ~50% split_precision em qualquer politica
LINE=$("$BIN/sim" "$TRACES/uniform_1001.bin" $VM_BLOCKS 0.50 aging 2000 $PRE_PERIODS $POST_ACCESSES "t,")
SP=$(echo "$LINE" | cut -d, -f4)
check "uniforme RAM50% aging split_precision~0.50" "$SP" 'awk -v x="$SP" "BEGIN{exit !(x>0.45 && x<0.55)}"'

# 2) sequencial ciclica > RAM, LRU exato -> ~0% de acerto apos aquecimento (medido via hit rate global)
LINE=$("$BIN/sim" "$TRACES/sequential.bin" $VM_BLOCKS 0.50 lru 2000 $PRE_PERIODS $POST_ACCESSES "t,")
PAGEINS=$(echo "$LINE" | cut -d, -f3)
HITRATE=$(awk -v p="$PAGEINS" -v t="$POST_ACCESSES" 'BEGIN{printf "%.4f", 1 - p/t}')
check "sequencial>RAM LRU exato hit_rate~0" "$HITRATE" 'awk -v x="$HITRATE" "BEGIN{exit !(x<0.05)}"'

# 3) qualquer carga: page-ins OPT <= page-ins das demais
L_OPT=$("$BIN/sim" "$TRACES/zipf_0.99_1001.bin" $VM_BLOCKS 0.50 opt 2000 $PRE_PERIODS $POST_ACCESSES "t,")
L_LRU=$("$BIN/sim" "$TRACES/zipf_0.99_1001.bin" $VM_BLOCKS 0.50 lru 2000 $PRE_PERIODS $POST_ACCESSES "t,")
L_AGE=$("$BIN/sim" "$TRACES/zipf_0.99_1001.bin" $VM_BLOCKS 0.50 aging 2000 $PRE_PERIODS $POST_ACCESSES "t,")
P_OPT=$(echo "$L_OPT" | cut -d, -f3); P_LRU=$(echo "$L_LRU" | cut -d, -f3); P_AGE=$(echo "$L_AGE" | cut -d, -f3)
check "zipf0.99 RAM50% page_ins(OPT)<=page_ins(LRU,aging)" "OPT=$P_OPT LRU=$P_LRU AGING=$P_AGE" "[ $P_OPT -le $P_LRU ] && [ $P_OPT -le $P_AGE ]"

# 4) determinismo: mesmo trace/semente/politica -> resultado identico
L1=$("$BIN/sim" "$TRACES/bursty_1001.bin" $VM_BLOCKS 0.50 chunkq 2000 $PRE_PERIODS $POST_ACCESSES "t,")
L2=$("$BIN/sim" "$TRACES/bursty_1001.bin" $VM_BLOCKS 0.50 chunkq 2000 $PRE_PERIODS $POST_ACCESSES "t,")
V1=$(echo "$L1" | cut -d, -f2-5); V2=$(echo "$L2" | cut -d, -f2-5)
check "determinismo (policy,page_ins,split,lru_agree identicos)" "$V1 vs $V2" '[ "$V1" = "$V2" ]'

if grep -q FAIL "$SANITY_LOG"; then
    echo "ALGUMA VERIFICACAO DE SANIDADE FALHOU -- interrompendo antes da bateria."
    exit 1
fi

echo "== bateria fatorial completa =="
CSV="$RESULTS/battery.csv"
echo "workload,zipf_alpha,ram_fraction,period_density,seed,vm_blocks,policy,page_ins,split_precision,lru_agreement,decision_time_ms,post_accesses,eviction_count,period_size,command" > "$CSV"

run_one() { # workload zipf_alpha trace_file seed
    local workload="$1" alpha="$2" trace="$3" seed="$4"
    for rf in $RAM_FRACTIONS; do
        for pd in $PERIODS; do
            local plabel="${pd%%:*}" psize="${pd##*:}"
            for pol in $POLICIES; do
                local meta="$workload,$alpha,$rf,$plabel,$seed,$VM_BLOCKS,"
                local cmd="bin/sim traces/$(basename "$trace") $VM_BLOCKS $rf $pol $psize $PRE_PERIODS $POST_ACCESSES"
                local out
                out=$("$BIN/sim" "$trace" $VM_BLOCKS "$rf" "$pol" "$psize" $PRE_PERIODS $POST_ACCESSES "$meta")
                echo "${out},${psize},\"${cmd}\"" >> "$CSV"
            done
        done
    done
}

run_one sequential "" "$TRACES/sequential.bin" ""
for a in 0.8 0.99 1.2; do
    for s in $SEEDS; do run_one "zipf" "$a" "$TRACES/zipf_${a}_${s}.bin" "$s"; done
done
for s in $SEEDS; do run_one uniform "" "$TRACES/uniform_${s}.bin" "$s"; done
for s in $SEEDS; do run_one phasechange "" "$TRACES/phasechange_${s}.bin" "$s"; done
for s in $SEEDS; do run_one bursty "" "$TRACES/bursty_${s}.bin" "$s"; done

echo "== bateria concluida =="
wc -l "$CSV"
