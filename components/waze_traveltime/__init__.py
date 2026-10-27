"""Waze travel time: the ESPHome side. register_plugin() reads the id, version, tile memory and the screen's texts from
the manifest and translations/ beside this folder; the tile, the card and the top bar's item are in waze_traveltime.cpp.
The item reads two entities of plugin.yaml: the travel time from Home Assistant and the usual time."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, sensor, smart_display
from esphome.const import CONF_ID

DEPENDENCIES = ["smart_display"]

CONF_MINUTES = "minutes"
CONF_USUAL = "usual"

waze_traveltime_ns = cg.esphome_ns.namespace("waze_traveltime")
WazeTraveltime = waze_traveltime_ns.class_("WazeTraveltime", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WazeTraveltime),
        cv.Required(CONF_MINUTES): cv.use_id(sensor.Sensor),
        cv.Required(CONF_USUAL): cv.use_id(number.Number),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_minutes(await cg.get_variable(config[CONF_MINUTES])))
    cg.add(var.set_usual(await cg.get_variable(config[CONF_USUAL])))
