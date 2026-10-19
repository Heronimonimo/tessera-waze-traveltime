"""Waze travel time: the ESPHome side. register_plugin() reads the id, version, tile memory and the screen's texts from
the manifest and translations/ beside this folder; the tile and the card are in waze_traveltime.cpp."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display"]

waze_traveltime_ns = cg.esphome_ns.namespace("waze_traveltime")
WazeTraveltime = waze_traveltime_ns.class_("WazeTraveltime", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(WazeTraveltime)}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
