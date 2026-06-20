package com.github.ulresh.homeaccounting.model

import com.fasterxml.jackson.core.JsonFactory
import com.fasterxml.jackson.core.JsonParser
import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.node.ArrayNode
import com.fasterxml.jackson.databind.node.ObjectNode
import java.io.InputStream

// Инкрементный разбор через библиотеку Jackson (streaming): парсер читает поток
// по мере необходимости и выдаёт значения по одному — целиком файл/блок в память
// не загружается. AUTO_CLOSE_SOURCE отключён, чтобы разбор сетевого блока не
// закрывал нижележащий сокет.
internal object Jk {
    val mapper = ObjectMapper()
    private val factory = JsonFactory().configure(JsonParser.Feature.AUTO_CLOSE_SOURCE, false)

    fun forEachValue(ins: InputStream, onValue: (JsonNode) -> Unit) {
        val it = mapper.readValues(factory.createParser(ins), JsonNode::class.java)
        try { while (it.hasNextValue()) onValue(it.nextValue()) } finally { it.close() }
    }

    fun parse(s: String): JsonNode = mapper.readTree(s)
    fun arr(): ArrayNode = mapper.createArrayNode()
    fun obj(): ObjectNode = mapper.createObjectNode()
    fun quote(s: String): String = mapper.writeValueAsString(s)   // верхнеуровневая строка -> "..."
}
