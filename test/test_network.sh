#!/bin/bash

# Проверка прав пользователя
if [[ $EUID -ne 0 ]]; then
   echo "Этот скрипт должен быть запущен от имени root."
   exit 1
fi

# Функция отображения подсказки
function usage() {
    echo "Использование: $0 <action> [options]"
    echo "Действия (action):"
    echo "  setup_network   - Настроить искажения по всей сети Docker"
    echo "  setup_container - Настроить искажения для конкретного контейнера"
    echo "  cleanup         - Очистить все правила"
    echo
    echo "Опции для action:"
    echo "  --container <имя_контейнера>   - Указать контейнер (только для setup_container)"
    echo "  --loss <процент>               - Вероятность потери пакетов (например, 20 для 20%)"
    echo "  --delay <задержка>             - Задержка в миллисекундах (например, 100ms)"
    echo "  --duplicate <процент>          - Вероятность дублирования пакетов (например, 5)"
    exit 1
}

# Проверка наличия утилиты tc
if ! command -v tc &> /dev/null; then
    echo "Ошибка: Утилита tc не установлена. Установите её и повторите попытку."
    exit 1
fi

# Параметры
ACTION=$1
shift
CONTAINER=""
LOSS="0%"
DELAY="0ms"
DUPLICATE="0%"

# Парсинг параметров
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --container) CONTAINER=$2; shift ;;
        --loss) LOSS=$2; shift ;;
        --delay) DELAY=$2; shift ;;
        --duplicate) DUPLICATE=$2; shift ;;
        *) echo "Неизвестный параметр: $1"; usage ;;
    esac
    shift
done

# Функция настройки для сети
function setup_network() {
    echo "=== Настройка сети с потерями: $LOSS, задержками: $DELAY, дублированием: $DUPLICATE ==="

    # Найти имя сети Docker
    NETWORK=$(docker network ls | grep my_network | awk '{print $2}')
    if [[ -z "$NETWORK" ]]; then
        echo "Ошибка: Сеть my_network не найдена. Убедитесь, что docker-compose запущен."
        exit 1
    fi

    # Найти интерфейс, связанный с сетью
    BRIDGE=$(docker network inspect "$NETWORK" | grep -m 1 'Id' | awk -F '"' '{print $4}')
    INTERFACE="br-${BRIDGE:0:12}"
    if [[ -z "$INTERFACE" ]]; then
        echo "Ошибка: Не удалось найти интерфейс сети Docker."
        exit 1
    fi

    # Применить настройки
    sudo tc qdisc add dev "$INTERFACE" root netem delay "$DELAY" loss "$LOSS" duplicate "$DUPLICATE"
    echo "Настройки применены для интерфейса $INTERFACE"
}

# Функция настройки для контейнера
function setup_container() {
    if [[ -z "$CONTAINER" ]]; then
        echo "Ошибка: Не указан контейнер. Используйте --container <имя_контейнера>."
        usage
    fi

    echo "=== Настройка контейнера $CONTAINER с потерями: $LOSS, задержками: $DELAY, дублированием: $DUPLICATE ==="

    # Получить PID контейнера
    PID=$(docker inspect -f '{{.State.Pid}}' "$CONTAINER")
    if [[ -z "$PID" ]]; then
        echo "Ошибка: Не удалось найти контейнер $CONTAINER."
        exit 1
    fi

    # Найти интерфейс контейнера
    NSENTER="sudo nsenter -n -t $PID"
    INTERFACE=$($NSENTER ip link | grep -o 'eth[0-9]*' | head -n 1)  # Обычно это eth0
    if [[ -z "$INTERFACE" ]]; then
        echo "Ошибка: Не удалось найти сетевой интерфейс контейнера $CONTAINER."
        exit 1
    fi

    # Применить настройки
    $NSENTER tc qdisc add dev "$INTERFACE" root netem delay "$DELAY" loss "$LOSS" duplicate "$DUPLICATE"
    echo "Настройки применены для интерфейса $INTERFACE внутри контейнера $CONTAINER"
}

# Функция очищения настроек
function cleanup() {
    echo "=== Очистка всех настроек TC ==="

    # Удалить настройки для сети Docker
    NETWORK=$(docker network ls | grep my_network | awk '{print $2}')
    if [[ -n "$NETWORK" ]]; then
        BRIDGE=$(docker network inspect "$NETWORK" | grep -m 1 'Id' | awk -F '"' '{print $4}')
        INTERFACE="br-${BRIDGE:0:12}"
        sudo tc qdisc del dev "$INTERFACE" root 2>/dev/null || true
        echo "Очистка сети $INTERFACE завершена"
    fi
# Очистить настройки контейнеров (опционально)
    if [[ -n "$CONTAINER" ]]; then
        PID=$(docker inspect -f '{{.State.Pid}}' "$CONTAINER")
        NSENTER="sudo nsenter -n -t $PID"
        INTERFACE=$($NSENTER ip link | grep -o 'eth[0-9]*' | head -n 1)
        if [[ -n "$INTERFACE" ]]; then
            $NSENTER tc qdisc del dev "$INTERFACE" root 2>/dev/null || true
            echo "Очистка контейнера $CONTAINER (интерфейс $INTERFACE) завершена"
        fi
    fi

    echo "Очистка завершена"
}

# Выполнение действия
case "$ACTION" in
    setup_network)
        setup_network
        ;;
    setup_container)
        setup_container
        ;;
    cleanup)
        cleanup
        ;;
    *)
        usage
        ;;
esac