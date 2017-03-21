#!/usr/bin/env python

import sys
import os
import os.path

separator = "/*" + ("*" * 50) + "*\n"
wmi_namespace_separator = "/"
wmi_classes_by_name = {}

class WmiClass:
    def __init__(self, name, variants = []):
        self.name = name
        self.variants = variants
        self.common = None

    def generate_classes_header(self):
        name_upper = self.name.upper()

        header = separator
        header += " * %s\n" % self.name
        header += " */\n"
        header += "\n"
        header += "#define %s_CLASSNAME \\\n" % name_upper
        header += "    \"%s\"\n" % self.name
        header += "\n"
        header += "#define %s_WQL_SELECT \\\n" % name_upper
        header += "    \"SELECT * FROM %s \"\n" % self.name
        header += "\n"
        header += "extern wmiClassInfoListPtr %s_WMI_Info;\n" % self.name

        # if there's more than one class variant, declare common data struct
        if self.common is not None:
            header += "struct _%s_Data {\n" % self.name
            for property in self.common:
                header += property.generate_classes_header()
            header += "};\n\n"

        # declare data struct for each variant
        for cls in self.variants:
            uri_info = cls.get_uri_info(self.name)
            header += "#define %s_RESOURCE_URI \\\n" % cls.name.upper()
            header += "    \"%s\"\n" % uri_info[1]
            header += "\n"
            header += "struct _%s_Data {\n" % cls.name
            for property in cls.properties:
                header += property.generate_classes_header()
            header += "};\n\n"
            header += "SER_DECLARE_TYPE(%s_Data);\n" % cls.name

        # declare hypervObject with data union member for each variant
        header += "\n/* must match hypervObject */\n"
        header += "struct _%s {\n" % self.name
        header += "    union {\n"
        if self.common is not None:
            header += "        %s_Data *common;\n" % self.name
        else:
            header += "        %s_Data *common;\n" % self.variants[0].name
        for cls in self.variants:
            header += "        %s_Data *%s;\n" % (cls.name, cls.namespace)
        header += "    } data;\n"
        header += "    wmiClassInfoPtr info;\n"
        header += "    %s *next;\n" % self.name
        header += "};\n"

        header += "\n\n\n"

        return header


    def generate_classes_source(self):
        source = separator
        source += " * %s\n" % self.name
        source += " */\n"

        for cls in self.variants:
            source += "SER_START_ITEMS(%s_Data)\n" % cls.name

            for property in cls.properties:
                source += property.generate_classes_source(cls.name)

            source += "SER_END_ITEMS(%s_Data);\n\n" % cls.name


        source += "wmiClassInfoListPtr %s_WMI_Info = &(wmiClassInfoList) {\n" % self.name
        source += "    %d, (wmiClassInfoPtr []) {\n" % len(self.variants)

        for cls in self.variants:
            uri_info = cls.get_uri_info(self.name)
            source += "        &(wmiClassInfo) {\n"
            source += "            %s_CLASSNAME,\n" % self.name.upper()
            source += "            %s,\n" % uri_info[0]
            source += "            %s_RESOURCE_URI,\n" % cls.name.upper()
            source += "            %s_Data_TypeInfo\n" % cls.name
            source += "        },\n"

        source += "    }\n"
        source += "};\n"

        source += "\n\n"

        return source


    def generate_classes_typedef(self):
        typedef = "typedef struct _%s %s;\n" % (self.name, self.name)

        if self.common is not None:
            typedef += "typedef struct _%s_Data %s_Data;\n" % (self.name, self.name)

        for cls in self.variants:
            typedef += "typedef struct _%s_Data %s_Data;\n" % (cls.name, cls.name)

        return typedef


    def align_struct_members(self):
        common = {}
        property_info = {}
        num_classes = len(self.variants)

        # if there's >1 variant, make sure it's name has namespace suffix
        if num_classes > 1:
            first = self.variants[0]
            first.name = "%s_%s" % (first.name, first.namespace)
        else:
            return

        # count property occurences in all class variants
        for cls in self.variants:
            for prop in cls.properties:
                key = "%s_%s" % (prop.name, prop.type)

                if key in property_info:
                    property_info[key][1] += 1
                else:
                    property_info[key] = [prop, 1]

        # isolate those that are common for all
        pos = 0
        for key in property_info:
            info = property_info[key]
            # exists in all class variants
            if info[1] == num_classes:
                common[info[0].name] = [info[0], pos]
                pos += 1

        # alter each variant's property list so that common members are first
        # and in the same order
        total = len(common)
        for cls in self.variants:
            index = 0
            count = len(cls.properties)

            while index < count:
                prop = cls.properties[index]

                if prop.name in common:
                    pos = common[prop.name][1]

                    # needs to be moved
                    if index != pos:
                        tmp = cls.properties[pos]
                        cls.properties[pos] = prop
                        cls.properties[index] = tmp
                    else:
                        index += 1
                else:
                    index += 1

        # finally, store common properites in a sorted list
        self.common = []
        tmp = sorted(common.values(), key=lambda x: x[1])
        for x in tmp:
            self.common.append(x[0])



class ClassVariant:
    def __init__(self, name, namespace, properties):
        self.name = name
        self.namespace = namespace
        self.properties = properties


    def get_uri_info(self, wmi_class_name):
        resourceUri = None
        rootUri = "ROOT_CIMV2"
        baseUri = "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/cimv2"

        if self.name.startswith("Msvm_"):
            baseUri = "http://schemas.microsoft.com/wbem/wsman/1/wmi/root/virtualization"
            rootUri = "ROOT_VIRTUALIZATION"
            if self.namespace == "v2":
                baseUri += "/v2"
                rootUri = "ROOT_VIRTUALIZATION_V2"

        resourceUri = "%s/%s" % (baseUri, wmi_class_name)

        return (rootUri, resourceUri)




class Property:
    typemap = {"boolean"  : "BOOL",
               "string"   : "STR",
               "datetime" : "STR",
               "int8"     : "INT8",
               "sint8"    : "INT8",
               "int16"    : "INT16",
               "sint16"   : "INT16",
               "int32"    : "INT32",
               "sint32"   : "INT32",
               "int64"    : "INT64",
               "sint64"   : "INT64",
               "uint8"    : "UINT8",
               "uint16"   : "UINT16",
               "uint32"   : "UINT32",
               "uint64"   : "UINT64"}


    def __init__(self, type, name, is_array):
        if type not in Property.typemap:
            report_error("unhandled property type %s" % type)

        self.type = type
        self.name = name
        self.is_array = is_array


    def generate_classes_header(self):
        if self.is_array:
            return "    XML_TYPE_DYN_ARRAY %s;\n" % self.name
        else:
            return "    XML_TYPE_%s %s;\n" \
                   % (Property.typemap[self.type], self.name)


    def generate_classes_source(self, class_name):
        if self.is_array:
            return "    SER_NS_DYN_ARRAY(%s_RESOURCE_URI, \"%s\", 0, 0, %s),\n" \
                   % (class_name.upper(), self.name, self.type)
        else:
            return "    SER_NS_%s(%s_RESOURCE_URI, \"%s\", 1),\n" \
                   % (Property.typemap[self.type], class_name.upper(), self.name)

def open_and_print(filename):
    if filename.startswith("./"):
        print "  GEN      " + filename[2:]
    else:
        print "  GEN      " + filename

    return open(filename, "wb")



def report_error(message):
    print "error: " + message
    sys.exit(1)



def parse_class(block):
    # expected format: class <name>
    header_items = block[0][1].split()

    if len(header_items) != 2:
        report_error("line %d: invalid block header" % (number))

    assert header_items[0] == "class"

    name = header_items[1]
    properties = []
    namespace = "v1"
    wmi_name = name
    ns_separator = name.find(wmi_namespace_separator)

    if ns_separator != -1:
        namespace = name[:ns_separator]
        wmi_name = name[ns_separator + 1:]
        name = "%s_%s" % (wmi_name, namespace)

    for line in block[1:]:
        # expected format: <type> <name>
        items = line[1].split()

        if len(items) != 2:
            report_error("line %d: invalid property" % line[0])

        if items[1].endswith("[]"):
            items[1] = items[1][:-2]
            is_array = True
        else:
            is_array = False

        properties.append(Property(type=items[0], name=items[1],
                                   is_array=is_array))

    cls = ClassVariant(name=name, namespace=namespace, properties=properties)

    if wmi_name in wmi_classes_by_name:
        wmi_classes_by_name[wmi_name].variants.append(cls)
    else:
        wmi_classes_by_name[wmi_name] = WmiClass(wmi_name, [cls])



def main():
    if "srcdir" in os.environ:
        input_filename = os.path.join(os.environ["srcdir"], "hyperv/hyperv_wmi_generator.input")
        output_dirname = os.path.join(os.environ["srcdir"], "hyperv")
    else:
        input_filename = os.path.join(os.getcwd(), "hyperv_wmi_generator.input")
        output_dirname = os.getcwd()


    classes_typedef = open_and_print(os.path.join(output_dirname, "hyperv_wmi_classes.generated.typedef"))
    classes_header = open_and_print(os.path.join(output_dirname, "hyperv_wmi_classes.generated.h"))
    classes_source = open_and_print(os.path.join(output_dirname, "hyperv_wmi_classes.generated.c"))


    number = 0
    block = None

    for line in file(input_filename, "rb").readlines():
        number += 1

        if "#" in line:
            line = line[:line.index("#")]

        line = line.lstrip().rstrip()

        if len(line) < 1:
                continue

        if line.startswith("class"):
            if block is not None:
                report_error("line %d: nested block found" % (number))
            else:
                block = []

        if block is not None:
            if line == "end":
                if block[0][1].startswith("class"):
                    parse_class(block)

                block = None
            else:
                block.append((number, line))

    names = wmi_classes_by_name.keys()
    names.sort()

    for name in names:
        cls = wmi_classes_by_name[name]
        cls.align_struct_members()
        classes_typedef.write(cls.generate_classes_typedef())
        classes_header.write(cls.generate_classes_header())
        classes_source.write(cls.generate_classes_source())



if __name__ == "__main__":
    main()
