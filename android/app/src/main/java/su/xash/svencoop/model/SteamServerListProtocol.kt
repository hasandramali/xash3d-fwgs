package su.xash.svencoop.model

import java.io.ByteArrayOutputStream

internal object SteamServerListProtocol {
    fun query(appid: Int): ByteArray {
        require(appid > 0)
        val output = ByteArrayOutputStream()
        fun varint(value: Int) {
            var remaining = value
            while (remaining and -128 != 0) {
                output.write((remaining and 127) or 128)
                remaining = remaining ushr 7
            }
            output.write(remaining)
        }
        varint(8); varint(appid)
        varint(24); varint(255)
        val filter = "\\appid\\$appid\\gamedir\\svencoop".toByteArray(Charsets.UTF_8)
        varint(34); varint(filter.size); output.write(filter)
        varint(40); varint(5000)
        return output.toByteArray()
    }

    private class Reader(private val data: ByteArray) {
        private var position = 0
        val remaining get() = data.size - position
        fun varint(): Long {
            var result = 0L
            for (shift in 0..63 step 7) {
                require(remaining > 0) { "Truncated protobuf varint" }
                val value = data[position++].toInt() and 255
                require(shift != 63 || value <= 1) { "Overflowing protobuf varint" }
                result = result or ((value and 127).toLong() shl shift)
                if (value and 128 == 0) return result
            }
            error("Invalid protobuf varint")
        }
        fun bytes(): ByteArray {
            val size = varint()
            require(size in 0..remaining.toLong()) { "Truncated protobuf field" }
            val result = data.copyOfRange(position, position + size.toInt())
            position += size.toInt()
            return result
        }
        fun fixed32(): Long {
            require(remaining >= 4)
            var value = 0L
            for (shift in 0..24 step 8) value = value or ((data[position++].toLong() and 255) shl shift)
            return value
        }
        fun skip(wire: Int) {
            when (wire) {
                0 -> varint()
                1 -> { require(remaining >= 8); position += 8 }
                2 -> bytes()
                5 -> fixed32()
                else -> error("Unsupported protobuf wire type")
            }
        }
    }

    private data class Endpoint(val ip: Long?, val queryPort: Int?, val gamePort: Int?)

    private fun endpoint(data: ByteArray): Endpoint {
        val reader = Reader(data)
        var ip: Long? = null
        var queryPort: Int? = null
        var gamePort: Int? = null
        while (reader.remaining > 0) {
            val tag = reader.varint().toInt()
            require(tag > 0)
            when (tag) {
                8 -> if (ip == null) ip = reader.varint() else reader.varint()
                16 -> queryPort = reader.varint().let { if (it in 1..65535) it.toInt() else 0 }
                72 -> gamePort = reader.varint().let { if (it in 1..65535) it.toInt() else 0 }
                34 -> {
                    val address = Reader(reader.bytes())
                    while (address.remaining > 0) {
                        val addressTag = address.varint().toInt()
                        require(addressTag > 0)
                        if (addressTag == 13) ip = address.fixed32() else address.skip(addressTag and 7)
                    }
                }
                else -> reader.skip(tag and 7)
            }
        }
        return Endpoint(ip, queryPort, gamePort)
    }

    fun servers(data: ByteArray): List<String> {
        require(data.size <= 4 * 1024 * 1024)
        val reader = Reader(data)
        val entries = ArrayList<Endpoint>()
        var defaults = Endpoint(null, null, null)
        while (reader.remaining > 0) {
            val tag = reader.varint().toInt()
            require(tag > 0)
            when (tag) {
                10 -> { require(entries.size < 5000); entries.add(endpoint(reader.bytes())) }
                18 -> {
                    val error = String(reader.bytes(), Charsets.UTF_8)
                    check(error.isEmpty()) { "Steam masterlist: $error" }
                }
                26 -> defaults = endpoint(reader.bytes())
                else -> reader.skip(tag and 7)
            }
        }
        val result = LinkedHashSet<String>()
        for (entry in entries) {
            val ip = entry.ip ?: defaults.ip ?: continue
            val port = entry.queryPort ?: defaults.queryPort ?: entry.gamePort ?: defaults.gamePort ?: continue
            if (ip <= 0 || ip >= 0xe0000000L || ip ushr 24 == 127L || port !in 1..65535) continue
            result.add("${ip ushr 24}.${(ip ushr 16) and 255}.${(ip ushr 8) and 255}.${ip and 255}:$port")
        }
        return result.toList()
    }
}
