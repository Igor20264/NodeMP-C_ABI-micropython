def on_join(player_id):
    name = node.get_player_name(player_id) or "?"
    node.log_info("hello, " + name)


def crunch(n):
    return n * (n + 1) // 2


def show(result, error):
    if error:
        node.log_error(error)
    else:
        info = node.core_info()
        node.log_info("sum = %s on core %s" % (result, info["index"]))


node.on("playerJoined", on_join)
node.job(crunch, 1000, done=show)


def unload():
    node.log_info("%s unloaded" % node.resource_name)
