# Функции `node`

Источник имён — `sdk/node.h`, ABI 2.8. Сервер NodeMP 1.12.0 говорит на ABI 2.7.
Предупреждение `NEWER SDK (2.8 vs 2.7)` в логе значит именно это: хост новее сервера.
Функция из раздела «нет на сервере 1.12.0» при вызове даёт ошибку, сервер от этого не падает.

Живой список в скрипте ресурса:

```python
names = sorted(name for name in dir(node) if not name.startswith('_'))
node.log_info(' '.join(names))
```

`dir(node)` показывает имена, которые объект реально имеет. Описание аргументов каждой функции NodeApi — комментарий над ней в `sdk/node.h`.

## Методы хоста

| Имя | Аргументы | Возврат | Смысл |
|---|---|---|---|
| `on` | name, fn | int | Слот подписки. Запрос, vehicle, builtin или клиентское. |
| `off` | slot | None | Снять событие. Таймер гасит cancel, не off. |
| `set_timeout` | ms, fn | int | Id таймера. В функцию приходит timer_id. |
| `set_interval` | ms, fn | int | Id таймера. |
| `after` | ms, fn | int | То же, что set_timeout. |
| `every` | ms, fn | int | То же, что set_interval. |
| `cancel` | timer_id | None | Остановить таймер. |
| `job` | fn, *args, done=None | None | fn на закреплённом ядре. done(result, error) на worker. |
| `core_info` | — | dict | index, processors, reserved, pinned, lane_id. |
| `on_core` | — | bool | Сейчас код на lane. |
| `reserve_core` | — | None | Снова снять ядро lane с остальных потоков. |
| `get_players` | — | list | Id игроков. |
| `get_vehicles` | — | list | Id машин. |
| `get_player_vehicles` | player_id | list | Машины игрока. |
| `get_player_vehicle` | player_id | tuple | (gid, role). |
| `get_vehicles_by_tag` | key, value=None | list | Машины с тегом. |
| `get_vehicles_by_tags` | pairs | list | Машины со всеми парами тегов. |
| `resource_name` | — | str | None | Поле. Имя текущего ресурса. |
| `resource_dir` | — | str | None | Поле. Папка ресурса. |
| `trusted` | — | bool | Поле. Флаг из server.toml. Дополнительных возможностей не открывает. |

## NodeApi, которые сервер 1.12.0 отдаёт

Строковые геттеры (в заголовке это пара `buf, buflen`) здесь уже без буфера: они возвращают `str` или `None`.

| Имя | Аргументы | Возврат | В SDK |
|---|---|---|---|
| `log_info` | msg | None | — |
| `log_warn` | msg | None | — |
| `log_error` | msg | None | — |
| `log_tag` | tag, msg | None | — |
| `log_custom` | tag, rgb, msg | None | — |
| `log_raw` | text | None | — |
| `set_console_title` | title | None | — |
| `get_player_count` | — | int | — |
| `get_player_name` | id | str \| None | — |
| `is_player_connected` | id | int | — |
| `get_player_ping` | id | int | — |
| `get_player_current_vehicle` | id | int | — |
| `kick_player` | id, reason | int | — |
| `get_vehicle_count` | — | int | — |
| `get_vehicle_damage_counter` | global_id | int | — |
| `get_vehicle_electrics_json` | global_id | str \| None | — |
| `delete_vehicle` | global_id | int | — |
| `delete_all_vehicles` | — | int | — |
| `set_vehicle_coupler` | global_id, name, open | int | — |
| `seat_player` | player_id, global_id, seat_kind | int | — |
| `unseat_player` | player_id | int | — |
| `swap_players` | player_a, player_b | int | — |
| `resync_vehicle` | global_id, player_id | int | — |
| `resync_player` | player_id | int | — |
| `set_vehicle_tag` | global_id, key, value | int | — |
| `get_vehicle_tag` | global_id, key | str \| None | — |
| `remove_vehicle_tag` | global_id, key | int | — |
| `get_vehicle_tags_json` | global_id | str \| None | — |
| `emit_client` | id, event, data | int | — |
| `emit_all` | event, data | None | — |
| `clear_timer` | timer_id | None | — |
| `get_server_name` | — | str \| None | — |
| `get_map` | — | str \| None | — |
| `get_server_version` | — | str \| None | — |
| `get_port` | — | int | — |
| `get_max_players` | — | int | — |
| `get_max_cars` | — | int | — |
| `get_player_ip` | id | str \| None | — |
| `get_player_port` | id | int | — |
| `set_player_role` | id, role | int | — |
| `get_player_role` | id | str \| None | — |
| `ban_player` | id, reason | int | — |
| `ban_ip` | ip, reason | int | — |
| `unban_ip` | ip | int | — |
| `is_ip_banned` | ip | int | — |
| `get_bans_json` | — | str \| None | 2.5 |
| `get_vehicle_config_json` | global_id | str \| None | — |
| `get_vehicle_couplers_json` | global_id | str \| None | — |
| `get_vehicle_powertrain_json` | global_id | str \| None | — |
| `get_vehicle_engine_json` | global_id | str \| None | — |
| `get_vehicle_controllers_json` | global_id | str \| None | — |
| `get_world_snapshot_json` | — | str \| None | — |
| `spawn_vehicle` | config_json, owner_name | int | — |
| `invalidate_relay_cache` | — | None | — |
| `storage_set` | store, key, value_json | int | — |
| `storage_get` | store, key | str \| None | — |
| `storage_delete` | store, key | int | — |
| `get_server_uptime` | — | float | — |
| `get_unix_time` | — | int | — |
| `set_max_cars` | n | int | — |
| `set_max_players` | n | int | — |
| `set_server_name` | name | int | — |
| `emit_resource_event` | name, data | None | — |
| `get_metrics_json` | — | str \| None | 2.3 |
| `trigger_vehicle` | global_id, call_json | int | — |
| `is_node_grab_enabled` | — | int | — |
| `unregister_language_host` | type_name | int | — |
| `set_resource_owner` | resource_name | None | — |
| `get_vehicle_json` | global_id | str \| None | 2.2 |
| `get_vehicles_json` | — | str \| None | — |
| `get_players_json` | — | str \| None | — |
| `get_player_endpoint_json` | id | str \| None | — |
| `get_player_position_json` | id | str \| None | — |
| `get_player_vehicle_json` | id | str \| None | — |
| `get_vehicle_damage_json` | global_id | str \| None | — |
| `get_vehicle_occupants_json` | global_id | str \| None | — |
| `reload_resource` | name | int | — |
| `get_vehicle_transform_json` | global_id | str \| None | — |
| `get_vehicle_controls_json` | global_id | str \| None | — |
| `get_vehicle_lock_json` | global_id | str \| None | — |
| `set_vehicle_lock_named` | global_id, mode, whitelist_json | int | — |
| `seat_player_role` | player_id, global_id, role | int | — |
| `set_player_group` | player_id, group | None | — |
| `set_vehicle_group` | global_id, group | None | — |
| `get_player_group` | player_id | int | — |
| `get_vehicle_group` | global_id | int | — |
| `emit_others` | except_player_id, event, data | int | 1.8 |
| `get_resource_manifest_json` | resource | str \| None | 1.8 |
| `get_vehicle_inputs_json` | global_id | str \| None | 1.9 |
| `get_player_inputs_json` | player_id | str \| None | 1.9 |
| `get_vehicle_nodes_json` | global_id | str \| None | 1.9 |
| `get_vehicle_break_groups_json` | global_id | str \| None | 1.9 |
| `get_vehicle_paints_json` | global_id | str \| None | 1.9 |
| `get_vehicle_sync_json` | global_id | str \| None | 1.9 |
| `get_vehicle_transforms_json` | — | str \| None | 1.9 |
| `get_player_focus_json` | player_id | str \| None | — |
| `get_player_head_pose_json` | player_id | str \| None | 1.9 |
| `get_player_camera_json` | player_id | str \| None | 1.12 |
| `get_player_session_json` | player_id | str \| None | 1.9 |
| `ban_account` | account_id, reason | int | 1.10 |
| `unban_account` | account_id | int | 1.10 |
| `is_account_banned` | account_id | int | 1.10 |
| `player_verify` | player_id, level | int | 1.12 |
| `player_set_strict` | player_id, on | int | 1.12 |
| `get_proto_version` | — | int | 2.1 |
| `release_vehicle_lock` | global_id | int | 2.2 |
| `get_player_policy_json` | player_id | str \| None | 2.2 |
| `reset_vehicle` | global_id, transform_json | int | 2.2 |
| `spawn_vehicle_for_player` | player_id, config_json, transform_json | int | — |
| `get_memory_json` | — | str \| None | 2.3 |
| `get_platform_json` | — | str \| None | 2.3 |
| `get_handler_stats_json` | reset | str \| None | 2.3 |
| `get_vehicle_transforms_select_json` | select | str \| None | 2.4 |
| `ban_player_until` | id, reason, until_unix | int | 2.5 |
| `ban_ip_until` | ip, reason, until_unix | int | — |
| `ban_account_until` | account_id, reason, until_unix | int | — |
| `shutdown_server` | reason, delay_ms | int | 2.5 |
| `teleport_vehicle` | global_id, transform_json | int | 2.5 |
| `freeze_player` | player_id, on | int | 2.5 |
| `is_resource_trusted` | name | int | 2.7 |

## Нет на сервере 1.12.0

Эти имена объявлены в SDK 2.8. Сервер 1.12.0 их не содержит.

| Имя | Аргументы | Возврат | В SDK |
|---|---|---|---|
| `set_vehicle_queue` | player_id, mode | int | 2.8 |
| `set_vehicle_queue_max_delay` | seconds | int | 2.8 |
| `apply_scheduled_vehicles` | player_id | int | 2.8 |
| `get_queue_json` | — | str \| None | 2.8 |
| `queue_move` | account_id, position | int | 2.8 |
| `queue_remove` | account_id, reason | int | 2.8 |
| `queue_admit` | account_id | int | 2.8 |
| `queue_pause` | paused, note | int | 2.8 |
| `get_queue_limits_json` | — | str \| None | 2.8 |
| `set_queue_limits_json` | limits_json | int | 2.8 |

## Есть в SDK, в этом хосте не обёрнуты

В MicroPython этих вызовов нет. Они есть у хоста `type = "python"`.

| Имя | Аргументы | Возврат | В SDK |
|---|---|---|---|
| `set_log_sink` | sink, user | None | — |
| `get_vehicle_info` | global_id, out | int | — |
| `get_vehicle_occupants` | global_id, ids, roles, max | int | — |
| `get_vehicle_transform` | global_id, out | int | — |
| `get_vehicle_controls` | global_id, out | int | — |
| `register_client_event` | event, cb, user | None | — |
| `unregister_client_event` | event, cb, user | None | — |
| `register_builtin_event` | name, cb, user | None | — |
| `unregister_builtin_event` | name, cb, user | None | — |
| `register_cancellable_event` | name, cb, user | None | — |
| `unregister_cancellable_event` | name, cb, user | None | — |
| `register_vehicle_event` | name, cb, user | None | — |
| `unregister_vehicle_event` | name, cb, user | None | — |
| `get_player_position` | id, out_pos | int | — |
| `get_vehicle_damage_blob` | global_id, buf, out_counter | int | — |
| `register_relay_filter` | cb, user | None | — |
| `unregister_relay_filter` | cb, user | None | — |
| `submit_job` | work, done, user | int | — |
| `http_request` | method, url, headers_json, body, body_len, cb, user | int | — |
| `register_resource_event` | name, cb, user | None | — |
| `unregister_resource_event` | name, cb, user | None | — |
| `sha256_hex` | data, len, out_hex65 | None | — |
| `hmac_sha256_hex` | key, key_len, data, data_len, out_hex65 | None | — |
| `random_bytes` | out, n | int | — |
| `send_module` | player_id, channel, data, len | int | — |
| `register_module_channel` | channel, cb, user | None | — |
| `unregister_module_channel` | channel, cb, user | None | — |
| `set_vehicle_lock` | global_id, mode, whitelist, count | int | 2.2 |
| `get_vehicle_lock` | global_id, out_whitelist, max, out_count | int | — |
| `get_vehicle_config_hash` | global_id, out_hash | int | — |
| `register_language_host` | host | int | — |
| `emit_client_bytes` | id, event, data, len | int | — |
| `emit_all_bytes` | event, data, len | None | — |
| `call_resource_event` | name, data, cb, user | int | 2.6 |
