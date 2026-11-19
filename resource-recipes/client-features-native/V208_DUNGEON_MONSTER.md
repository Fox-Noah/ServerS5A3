# V208 副本与怪物原生接口

接口绑定本仓库构建器校验的 V208 PE32 原版。地址为首选映像基址 0x00400000 下的虚拟地址。调用方须在游戏线程内借用仍然存活的原生对象，访问属性前检查对象布局。

## 开始地图

START_MAP（op29）回调注册位于 `0x02001f84`，原回调为 `0x01ff8d80`。适配器通过 `v208::ActivePacketReader()` 获取当前封包游标及剩余长度，再交回原回调。

```text
on_start_map():
    room_rules.reset()
    packet = active_packet_reader()
    if packet has a valid MAT1 footer and complete level intervals:
        room_rules.replace(decoded_rates)
        packet.remaining = original_body_length
    continue native_start_map(packet)
```

MAT1 是怪物 AI 扩展。每行由起始等级、结束等级及六个小端 binary64 倍率组成，末尾是小端行数据长度和 MAT1 标记。等级区间覆盖 0–255。下一次 START_MAP 替换当前快照，普通封包恢复默认倍率。

## 怪物出生与属性

副本创建路径调用工厂 `0x030d1960`，随后由 `0x02628d60` 设置房间对象。此处的正常副本生成路径已经核对。钩子位于 `0x02628edd`，此时对象已经建立，原函数随后读取当前血量作为出生快照。

```text
on_room_actor_created(actor):
    if actor has supported monster layout:
        level = read_native_scalar(base_attribute_block.level)
        rates = room_rules.for_level(level)
        for each of four attribute blocks:
            scale maximum_health, move_speed, attack_speed
            scale physical_attack and magical_attack
            scale physical_defense and magical_defense
        scale current_health
        scale aggression and clamp to 0..100
    resume native spawn at 0x02628ee5
```

[v208_dungeon_monster.h](native/v208_dungeon_monster.h) 提供 `PacketReader`、`MonsterView` 和原生属性读写。普通数值用 32 位原生编解码器，血量用 64 位原生编解码器。最大血量分布在四个属性块，当前血量单独存储。接口中的属性块和字段指针仅在对象存活期间使用。其他客户端版本或未通过布局检查的怪物需单独核对。

## 构建

在 Windows 上使用 MSVC x86、Python 和 pefile。

```bat
native\build_monster_tuning_v208.bat "原版DNF.exe路径" "新的输出目录"
```

构建基于已编译的原版 EXE。现有怪物 AI 适配器复用此接口，生成 `DNF-receiver.exe`。联机验收使用配套服务端的 `DNFBRIDGE_MONSTER_TUNING=paired-v1`。
