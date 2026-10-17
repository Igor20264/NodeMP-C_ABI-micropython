# Тот же контракт запроса, что у CPython: None разрешает, строка запрещает.
# Тяжёлая сумма уходит в node.job и не держит worker.

BLOCKED = {"Player9"}
CAR_LIMIT = 2


def on_connect(player_id, global_id, name):
    if name in BLOCKED:
        return "You are not allowed on this server"
    node.log_info("micropython-gate: " + name + " passed the connect gate")
    return None


def on_spawn(player_id, global_id, config):
    owned = node.get_player_vehicles(player_id)
    if len(owned) >= CAR_LIMIT:
        return "Vehicle limit reached (%s per player)" % CAR_LIMIT
    name = node.get_player_name(player_id) or "?"
    node.log_info("micropython-gate: %s spawns a vehicle (%s/%s)" % (name, len(owned) + 1, CAR_LIMIT))
    return None


def fold(n):
    return n * (n + 1) // 2


def show(result, error):
    if error:
        node.log_error(error)
        return
    info = node.core_info()
    node.log_info("micropython-gate: sum %s on core %s" % (result, info["index"]))


def greet(player_id):
    name = node.get_player_name(player_id) or "?"
    node.log_info("micropython-gate: " + name + " joined")
    node.emit_client(player_id, "greeting", "Welcome, " + name)
    node.job(fold, 1000, done=show)


def on_leave(player_id, global_id, data):
    name = node.get_player_name(player_id) or "?"
    node.log_info("micropython-gate: " + name + " left")


def on_minute(timer_id):
    node.log_info("micropython-gate: %s players online" % node.get_player_count())


node.on("playerConnectRequest", on_connect)
node.on("vehicleSpawnRequest", on_spawn)
node.on("playerJoined", greet)
# node.on("playerLeft") берёт форму с тремя аргументами, не с одним.
node.on("playerLeft", on_leave)
clock = node.every(60000, on_minute)

for player_id in node.get_players():
    greet(player_id)

node.log_info("micropython-gate loaded")


def unload():
    node.cancel(clock)
    node.log_info("micropython-gate unloaded")
