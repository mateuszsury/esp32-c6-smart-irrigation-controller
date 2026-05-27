// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mateusz Sury
//
// Zigbee2MQTT external converter for ESP32-C6 Smart Irrigation Controller.

const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');

const e = exposes.presets;
const ea = exposes.access;

const IRRIGATION_CLUSTER = 0xFC00;
const MANUFACTURER_CODE = 0x131B;
const ATTR_DURATION_S = 0x0001;
const ATTR_INTERLOCK = 0x0002;
const ATTR_LINE_COUNT = 0x0003;
const ATTR_SCHEDULE_ENABLED = 0x0010;
const ATTR_SCHEDULE_START_MINUTE = 0x0011;
const ATTR_SCHEDULE_WEEKDAYS = 0x0012;

const lineEndpoints = {
    line_1: 1,
    line_2: 2,
    line_3: 3,
};

const durationKeys = Object.fromEntries(
    Object.entries(lineEndpoints).map(([name, endpoint]) => [`duration_${name}`, endpoint]),
);
const scheduleEnabledKeys = Object.fromEntries(
    Object.entries(lineEndpoints).map(([name, endpoint]) => [`schedule_enabled_${name}`, endpoint]),
);
const scheduleStartMinuteKeys = Object.fromEntries(
    Object.entries(lineEndpoints).map(([name, endpoint]) => [`schedule_start_minute_${name}`, endpoint]),
);
const scheduleWeekdaysKeys = Object.fromEntries(
    Object.entries(lineEndpoints).map(([name, endpoint]) => [`schedule_weekdays_${name}`, endpoint]),
);
const scheduleDays = [
    {key: 'schedule_monday', bit: 0x02, label: 'Monday'},
    {key: 'schedule_tuesday', bit: 0x04, label: 'Tuesday'},
    {key: 'schedule_wednesday', bit: 0x08, label: 'Wednesday'},
    {key: 'schedule_thursday', bit: 0x10, label: 'Thursday'},
    {key: 'schedule_friday', bit: 0x20, label: 'Friday'},
    {key: 'schedule_saturday', bit: 0x40, label: 'Saturday'},
    {key: 'schedule_sunday', bit: 0x01, label: 'Sunday'},
];
const scheduleWeekdayKeys = Object.fromEntries(
    scheduleDays.flatMap((day) =>
        Object.entries(lineEndpoints).map(([name, endpoint]) => [`${day.key}_${name}`, {endpoint, day}]),
    ),
);

function endpointFromMeta(entity, meta) {
    if (meta && meta.endpoint_name && lineEndpoints[meta.endpoint_name]) {
        return lineEndpoints[meta.endpoint_name];
    }
    if (entity && entity.ID) {
        return entity.ID;
    }
    return 1;
}

function attr(data, id) {
    return data[id] ?? data[String(id)];
}

function lineName(endpointId) {
    return `line_${endpointId}`;
}

function scheduleDayState(mask, endpointId) {
    const result = {};
    for (const day of scheduleDays) {
        result[`${day.key}_${lineName(endpointId)}`] = (Number(mask) & day.bit) !== 0;
    }
    return result;
}

function currentWeekdaysMask(meta, endpointId) {
    const state = meta && meta.state ? meta.state : {};
    const name = lineName(endpointId);
    const raw = Number(state[`schedule_weekdays_${name}`]);
    if (Number.isInteger(raw) && raw >= 0 && raw <= 127) {
        return raw;
    }

    let mask = 0;
    let known = false;
    for (const day of scheduleDays) {
        const value = state[`${day.key}_${name}`];
        if (typeof value === 'boolean') {
            known = true;
            if (value) {
                mask |= day.bit;
            }
        }
    }
    return known ? mask : 127;
}

const fzIrrigationConfig = {
    cluster: IRRIGATION_CLUSTER,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const result = {};
        const duration = attr(msg.data, ATTR_DURATION_S);
        const interlock = attr(msg.data, ATTR_INTERLOCK);
        const lineCount = attr(msg.data, ATTR_LINE_COUNT);
        const scheduleEnabled = attr(msg.data, ATTR_SCHEDULE_ENABLED);
        const scheduleStartMinute = attr(msg.data, ATTR_SCHEDULE_START_MINUTE);
        const scheduleWeekdays = attr(msg.data, ATTR_SCHEDULE_WEEKDAYS);

        if (duration !== undefined) {
            result[`duration_line_${msg.endpoint.ID}`] = Number(duration);
        }
        if (interlock !== undefined) {
            result.interlock = Boolean(interlock);
        }
        if (lineCount !== undefined) {
            result.line_count = Number(lineCount);
        }
        if (scheduleEnabled !== undefined) {
            result[`schedule_enabled_line_${msg.endpoint.ID}`] = Boolean(scheduleEnabled);
        }
        if (scheduleStartMinute !== undefined) {
            result[`schedule_start_minute_line_${msg.endpoint.ID}`] = Number(scheduleStartMinute);
        }
        if (scheduleWeekdays !== undefined) {
            const weekdays = Number(scheduleWeekdays);
            result[`schedule_weekdays_line_${msg.endpoint.ID}`] = weekdays;
            Object.assign(result, scheduleDayState(weekdays, msg.endpoint.ID));
        }
        return result;
    },
};

const tzIrrigationConfig = {
    key: [
        'duration',
        ...Object.keys(durationKeys),
        'schedule_enabled',
        ...Object.keys(scheduleEnabledKeys),
        'schedule_start_minute',
        ...Object.keys(scheduleStartMinuteKeys),
        'schedule_weekdays',
        ...Object.keys(scheduleWeekdaysKeys),
        ...scheduleDays.map((day) => day.key),
        ...Object.keys(scheduleWeekdayKeys),
        'interlock',
    ],
    convertSet: async (entity, key, value, meta) => {
        if (key === 'interlock') {
            const endpoint = meta.device.getEndpoint(1);
            await endpoint.write(
                IRRIGATION_CLUSTER,
                {[ATTR_INTERLOCK]: {value: Boolean(value), type: 0x10}},
                {manufacturerCode: MANUFACTURER_CODE},
            );
            return {state: {interlock: Boolean(value)}};
        }

        if (key === 'schedule_enabled' || scheduleEnabledKeys[key]) {
            const endpointId = scheduleEnabledKeys[key] || endpointFromMeta(entity, meta);
            const enabled = Boolean(value);
            const endpoint = meta.device.getEndpoint(endpointId);
            await endpoint.write(
                IRRIGATION_CLUSTER,
                {[ATTR_SCHEDULE_ENABLED]: {value: enabled, type: 0x10}},
                {manufacturerCode: MANUFACTURER_CODE},
            );
            const stateKey = meta && meta.endpoint_name ? 'schedule_enabled' : key;
            return {state: {[stateKey]: enabled}};
        }

        if (key === 'schedule_start_minute' || scheduleStartMinuteKeys[key]) {
            const endpointId = scheduleStartMinuteKeys[key] || endpointFromMeta(entity, meta);
            const startMinute = Number(value);
            if (!Number.isInteger(startMinute) || startMinute < 0 || startMinute > 1439) {
                throw new Error(`${key} must be an integer from 0 to 1439`);
            }
            const endpoint = meta.device.getEndpoint(endpointId);
            await endpoint.write(
                IRRIGATION_CLUSTER,
                {[ATTR_SCHEDULE_START_MINUTE]: {value: startMinute, type: 0x21}},
                {manufacturerCode: MANUFACTURER_CODE},
            );
            const stateKey = meta && meta.endpoint_name ? 'schedule_start_minute' : key;
            return {state: {[stateKey]: startMinute}};
        }

        if (key === 'schedule_weekdays' || scheduleWeekdaysKeys[key]) {
            const endpointId = scheduleWeekdaysKeys[key] || endpointFromMeta(entity, meta);
            const weekdays = Number(value);
            if (!Number.isInteger(weekdays) || weekdays < 0 || weekdays > 127) {
                throw new Error(`${key} must be an integer from 0 to 127`);
            }
            const endpoint = meta.device.getEndpoint(endpointId);
            await endpoint.write(
                IRRIGATION_CLUSTER,
                {[ATTR_SCHEDULE_WEEKDAYS]: {value: weekdays, type: 0x20}},
                {manufacturerCode: MANUFACTURER_CODE},
            );
            const stateKey = meta && meta.endpoint_name ? 'schedule_weekdays' : key;
            return {state: {[stateKey]: weekdays, ...scheduleDayState(weekdays, endpointId)}};
        }

        const directDay = scheduleDays.find((day) => day.key === key);
        const endpointDay = scheduleWeekdayKeys[key];
        if (directDay || endpointDay) {
            const endpointId = endpointDay ? endpointDay.endpoint : endpointFromMeta(entity, meta);
            const day = endpointDay ? endpointDay.day : directDay;
            let weekdays = currentWeekdaysMask(meta, endpointId);
            if (Boolean(value)) {
                weekdays |= day.bit;
            } else {
                weekdays &= ~day.bit;
            }
            const endpoint = meta.device.getEndpoint(endpointId);
            await endpoint.write(
                IRRIGATION_CLUSTER,
                {[ATTR_SCHEDULE_WEEKDAYS]: {value: weekdays, type: 0x20}},
                {manufacturerCode: MANUFACTURER_CODE},
            );
            const stateKey = meta && meta.endpoint_name ? day.key : key;
            return {state: {[stateKey]: Boolean(value), [`schedule_weekdays_${lineName(endpointId)}`]: weekdays}};
        }

        const endpointId = durationKeys[key] || endpointFromMeta(entity, meta);
        const duration = Number(value);
        if (!Number.isInteger(duration) || duration < 1 || duration > 86400) {
            throw new Error(`${key} must be an integer from 1 to 86400 seconds`);
        }
        const endpoint = meta.device.getEndpoint(endpointId);
        await endpoint.write(
            IRRIGATION_CLUSTER,
            {[ATTR_DURATION_S]: {value: duration, type: 0x23}},
            {manufacturerCode: MANUFACTURER_CODE},
        );
        const stateKey = meta && meta.endpoint_name ? 'duration' : (durationKeys[key] ? key : 'duration');
        return {state: {[stateKey]: duration}};
    },
    convertGet: async (entity, key, meta) => {
        if (key === 'interlock') {
            await meta.device.getEndpoint(1).read(IRRIGATION_CLUSTER, [ATTR_INTERLOCK], {manufacturerCode: MANUFACTURER_CODE});
            return;
        }
        if (key === 'schedule_enabled' || scheduleEnabledKeys[key]) {
            const endpointId = scheduleEnabledKeys[key] || endpointFromMeta(entity, meta);
            await meta.device.getEndpoint(endpointId).read(IRRIGATION_CLUSTER, [ATTR_SCHEDULE_ENABLED], {manufacturerCode: MANUFACTURER_CODE});
            return;
        }
        if (key === 'schedule_start_minute' || scheduleStartMinuteKeys[key]) {
            const endpointId = scheduleStartMinuteKeys[key] || endpointFromMeta(entity, meta);
            await meta.device.getEndpoint(endpointId).read(IRRIGATION_CLUSTER, [ATTR_SCHEDULE_START_MINUTE], {manufacturerCode: MANUFACTURER_CODE});
            return;
        }
        if (key === 'schedule_weekdays' || scheduleWeekdaysKeys[key]) {
            const endpointId = scheduleWeekdaysKeys[key] || endpointFromMeta(entity, meta);
            await meta.device.getEndpoint(endpointId).read(IRRIGATION_CLUSTER, [ATTR_SCHEDULE_WEEKDAYS], {manufacturerCode: MANUFACTURER_CODE});
            return;
        }
        if (scheduleDays.some((day) => day.key === key) || scheduleWeekdayKeys[key]) {
            const endpointId = scheduleWeekdayKeys[key] ? scheduleWeekdayKeys[key].endpoint : endpointFromMeta(entity, meta);
            await meta.device.getEndpoint(endpointId).read(IRRIGATION_CLUSTER, [ATTR_SCHEDULE_WEEKDAYS], {manufacturerCode: MANUFACTURER_CODE});
            return;
        }
        const endpointId = durationKeys[key] || endpointFromMeta(entity, meta);
        await meta.device.getEndpoint(endpointId).read(IRRIGATION_CLUSTER, [ATTR_DURATION_S], {manufacturerCode: MANUFACTURER_CODE});
    },
};

function durationExpose(endpointName) {
    return exposes.numeric('duration', ea.ALL)
        .withEndpoint(endpointName)
        .withUnit('s')
        .withValueMin(1)
        .withValueMax(86400);
}

function scheduleEnabledExpose(endpointName) {
    return exposes.binary('schedule_enabled', ea.ALL, true, false).withEndpoint(endpointName);
}

function scheduleStartMinuteExpose(endpointName) {
    return exposes.numeric('schedule_start_minute', ea.ALL)
        .withEndpoint(endpointName)
        .withUnit('min')
        .withValueMin(0)
        .withValueMax(1439);
}

function scheduleWeekdaysExpose(endpointName) {
    return exposes.numeric('schedule_weekdays', ea.ALL)
        .withEndpoint(endpointName)
        .withValueMin(0)
        .withValueMax(127);
}

function scheduleWeekdayExpose(endpointName, day) {
    return exposes.binary(day.key, ea.ALL, true, false)
        .withEndpoint(endpointName)
        .withDescription(`Run schedule on ${day.label}`);
}

module.exports = [
    {
        zigbeeModel: ['ESP32-C6 Irrigation Controller'],
        model: 'esp32c6-irrigation-controller',
        vendor: 'Mateusz Sury',
        description: 'ESP32-C6 multi-line irrigation controller',
        fromZigbee: [fz.on_off, fzIrrigationConfig],
        toZigbee: [tz.on_off, tzIrrigationConfig],
        exposes: [
            e.switch().withEndpoint('line_1'),
            e.switch().withEndpoint('line_2'),
            e.switch().withEndpoint('line_3'),
            durationExpose('line_1'),
            durationExpose('line_2'),
            durationExpose('line_3'),
            scheduleEnabledExpose('line_1'),
            scheduleEnabledExpose('line_2'),
            scheduleEnabledExpose('line_3'),
            scheduleStartMinuteExpose('line_1'),
            scheduleStartMinuteExpose('line_2'),
            scheduleStartMinuteExpose('line_3'),
            ...scheduleDays.map((day) => scheduleWeekdayExpose('line_1', day)),
            ...scheduleDays.map((day) => scheduleWeekdayExpose('line_2', day)),
            ...scheduleDays.map((day) => scheduleWeekdayExpose('line_3', day)),
            exposes.binary('interlock', ea.ALL, true, false),
        ],
        endpoint: () => lineEndpoints,
        meta: {multiEndpoint: true},
        configure: async (device, coordinatorEndpoint) => {
            for (const endpoint of device.endpoints.filter((ep) => ep.ID >= 1 && ep.ID <= 8)) {
                await reporting.bind(endpoint, coordinatorEndpoint, ['genOnOff']);
                await reporting.onOff(endpoint);
                await endpoint.read(IRRIGATION_CLUSTER, [ATTR_DURATION_S, ATTR_INTERLOCK, ATTR_LINE_COUNT], {
                    manufacturerCode: MANUFACTURER_CODE,
                });
                await endpoint.read(
                    IRRIGATION_CLUSTER,
                    [ATTR_SCHEDULE_ENABLED, ATTR_SCHEDULE_START_MINUTE, ATTR_SCHEDULE_WEEKDAYS],
                    {manufacturerCode: MANUFACTURER_CODE},
                );
            }
        },
    },
];
