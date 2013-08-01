#ifndef OSMIUM_IO_XML_INPUT_HPP
#define OSMIUM_IO_XML_INPUT_HPP

/*

This file is part of Osmium (http://osmcode.org/osmium).

Copyright 2013 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#define OSMIUM_LINK_WITH_LIBS_EXPAT -lexpat

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <expat.h>

#include <osmium/io/input.hpp>
#include <osmium/osm/builder.hpp>
#include <osmium/thread/queue.hpp>

namespace osmium {

    namespace io {

        class XMLParser {

            static constexpr int xml_buffer_size = 10240;
            static constexpr int buffer_size = 10 * 1000 * 1000;

            enum class context {
                root,
                top,
                node,
                way,
                relation,
                in_object
            };

            context m_context;
            context m_last_context;

            /**
             * This is used only for change files which contain create, modify,
             * and delete sections.
             */
            bool m_in_delete_section;

            int m_fd;

            osmium::io::Meta m_meta;

            osmium::memory::Buffer m_buffer;

            osmium::memory::NodeBuilder*               m_node_builder;
            osmium::memory::WayBuilder*                m_way_builder;
            osmium::memory::RelationBuilder*           m_relation_builder;
            osmium::memory::TagListBuilder*            m_tl_builder;
            osmium::memory::WayNodeListBuilder*        m_wnl_builder;
            osmium::memory::RelationMemberListBuilder* m_rml_builder;

            osmium::thread::Queue<osmium::memory::Buffer>& m_queue;
            std::promise<osmium::io::Meta>& m_meta_promise;

            bool m_promise_fulfilled;

            size_t m_max_queue_size;

        public:

            XMLParser(int fd, osmium::thread::Queue<osmium::memory::Buffer>& queue, std::promise<osmium::io::Meta>& meta_promise) :
                m_context(context::root),
                m_last_context(context::root),
                m_in_delete_section(false),
                m_fd(fd),
                m_meta(),
                m_buffer(new char[buffer_size], buffer_size, 0),
                m_node_builder(nullptr),
                m_way_builder(nullptr),
                m_relation_builder(nullptr),
                m_tl_builder(nullptr),
                m_wnl_builder(nullptr),
                m_rml_builder(nullptr),
                m_queue(queue),
                m_meta_promise(meta_promise),
                m_promise_fulfilled(false),
                m_max_queue_size(100) {
            }

            void operator()() {
                XML_Parser parser = XML_ParserCreate(0);
                if (!parser) {
                    throw std::runtime_error("Error creating parser");
                }

                XML_SetUserData(parser, this);

                XML_SetElementHandler(parser, start_element_wrapper, end_element_wrapper);

                int done;
                do {
                    void* buffer = XML_GetBuffer(parser, xml_buffer_size);
                    if (buffer == nullptr) {
                        throw std::runtime_error("out of memory");
                    }

                    ssize_t result = ::read(m_fd, buffer, xml_buffer_size);
                    if (result < 0) {
                        throw std::runtime_error("read error");
                    }
                    done = (result == 0);
                    if (XML_ParseBuffer(parser, result, done) == XML_STATUS_ERROR) {
                        XML_Error errorCode = XML_GetErrorCode(parser);
                        long errorLine = XML_GetCurrentLineNumber(parser);
                        long errorCol = XML_GetCurrentColumnNumber(parser);
                        const XML_LChar* errorString = XML_ErrorString(errorCode);

                        std::stringstream errorDesc;
                        errorDesc << "XML parsing error at line " << errorLine << ":" << errorCol;
                        errorDesc << ": " << errorString;
                        throw std::runtime_error(errorDesc.str());
                    }
                } while (!done);
                XML_ParserFree(parser);
            }

        private:

            static void XMLCALL start_element_wrapper(void* data, const XML_Char* element, const XML_Char** attrs) {
                static_cast<XMLParser*>(data)->start_element(element, attrs);
            }

            static void XMLCALL end_element_wrapper(void* data, const XML_Char* element) {
                static_cast<XMLParser*>(data)->end_element(element);
            }

            void init_object(osmium::memory::Builder* builder, osmium::Object& object, const XML_Char** attrs) {
                bool user_set = false;
                if (m_in_delete_section) {
                    object.visible(false);
                }
                for (int count = 0; attrs[count]; count += 2) {
                    if (!strcmp(attrs[count], "lon")) {
                        static_cast<osmium::Node&>(object).lon(atof(attrs[count+1])); // XXX
                    } else if (!strcmp(attrs[count], "lat")) {
                        static_cast<osmium::Node&>(object).lat(atof(attrs[count+1])); // XXX
                    } else if (!strcmp(attrs[count], "user")) {
                        builder->add_string(attrs[count+1]);
                        user_set = true;
                    } else {
                        object.set_attribute(attrs[count], attrs[count+1]);
                    }
                }

                if (!user_set) {
                    builder->add_string("");
                }
//                std::cerr << "node id=" << object.id() << std::endl;
            }

            void check_tag(osmium::memory::Builder* builder, const XML_Char* element, const XML_Char** attrs) {
                if (!strcmp(element, "tag")) {
                    close_wnl_builder();
                    close_rml_builder();

                    const char* key = "";
                    const char* value = "";
                    for (int count = 0; attrs[count]; count += 2) {
                        if (attrs[count][0] == 'k' && attrs[count][1] == 0) {
                            key = attrs[count+1];
                        }
                        if (attrs[count][0] == 'v' && attrs[count][1] == 0) {
                            value = attrs[count+1];
                        }
                    }
                    if (!m_tl_builder) {
                        m_tl_builder = new osmium::memory::TagListBuilder(m_buffer, builder);
                    }
                    m_tl_builder->add_tag(key, value);
                }
            }

            void start_element(const XML_Char* element, const XML_Char** attrs) {
                try {
                    switch (m_context) {
                        case context::root:
                            if (!strcmp(element, "osm") || !strcmp(element, "osmChange")) {
                                for (int count = 0; attrs[count]; count += 2) {
                                    if (!strcmp(attrs[count], "version")) {
                                        if (strcmp(attrs[count+1], "0.6")) {
                                            throw std::runtime_error("can only read version 0.6 files");
                                        }
                                    } else if (!strcmp(attrs[count], "generator")) {
                                        m_meta.generator(attrs[count+1]);
                                    }
                                }
                            }
                            m_context = context::top;
                            break;
                        case context::top:
                            assert(m_tl_builder == nullptr);
                            if (!strcmp(element, "node")) {
                                if (!m_promise_fulfilled) {
                                    m_meta_promise.set_value(m_meta);
                                    m_promise_fulfilled = true;
                                }
                                m_node_builder = new osmium::memory::NodeBuilder(m_buffer);
                                init_object(m_node_builder, m_node_builder->object(), attrs);
                                m_context = context::node;
                            } else if (!strcmp(element, "way")) {
                                if (!m_promise_fulfilled) {
                                    m_meta_promise.set_value(m_meta);
                                    m_promise_fulfilled = true;
                                }
                                m_way_builder = new osmium::memory::WayBuilder(m_buffer);
                                init_object(m_way_builder, m_way_builder->object(), attrs);
                                m_context = context::way;
                            } else if (!strcmp(element, "relation")) {
                                if (!m_promise_fulfilled) {
                                    m_meta_promise.set_value(m_meta);
                                    m_promise_fulfilled = true;
                                }
                                m_relation_builder = new osmium::memory::RelationBuilder(m_buffer);
                                init_object(m_relation_builder, m_relation_builder->object(), attrs);
                                m_context = context::relation;
                            } else if (!strcmp(element, "bounds")) {
                                osmium::Location min;
                                osmium::Location max;
                                for (int count = 0; attrs[count]; count += 2) {
                                    if (!strcmp(attrs[count], "minlon")) {
                                        min.lon(atof(attrs[count+1]));
                                    } else if (!strcmp(attrs[count], "minlat")) {
                                        min.lat(atof(attrs[count+1]));
                                    } else if (!strcmp(attrs[count], "maxlon")) {
                                        max.lon(atof(attrs[count+1]));
                                    } else if (!strcmp(attrs[count], "maxlat")) {
                                        max.lat(atof(attrs[count+1]));
                                    }
                                }
                                m_meta.bounds().extend(min).extend(max);
                            } else if (!strcmp(element, "delete")) {
                                m_in_delete_section = true;
                            }
                            break;
                        case context::node:
                            m_last_context = context::node;
                            m_context = context::in_object;
                            check_tag(m_node_builder, element, attrs);
                            break;
                        case context::way:
                            m_last_context = context::way;
                            m_context = context::in_object;
                            if (!strcmp(element, "nd")) {
                                close_tl_builder();

                                if (!m_wnl_builder) {
                                    m_wnl_builder = new osmium::memory::WayNodeListBuilder(m_buffer, m_way_builder);
                                }

                                for (int count = 0; attrs[count]; count += 2) {
                                    if (!strcmp(attrs[count], "ref")) {
                                        m_wnl_builder->add_way_node(osmium::string_to_object_id(attrs[count+1]));
                                    }
                                }
                            } else {
                                check_tag(m_way_builder, element, attrs);
                            }
                            break;
                        case context::relation:
                            m_last_context = context::relation;
                            m_context = context::in_object;
                            if (!strcmp(element, "member")) {
                                close_tl_builder();

                                if (!m_rml_builder) {
                                    m_rml_builder = new osmium::memory::RelationMemberListBuilder(m_buffer, m_relation_builder);
                                }

                                char type = 'x';
                                object_id_type ref  = 0;
                                const char* role = "";
                                for (int count = 0; attrs[count]; count += 2) {
                                    if (!strcmp(attrs[count], "type")) {
                                        type = static_cast<char>(attrs[count+1][0]);
                                    } else if (!strcmp(attrs[count], "ref")) {
                                        ref = osmium::string_to_object_id(attrs[count+1]);
                                    } else if (!strcmp(attrs[count], "role")) {
                                        role = static_cast<const char*>(attrs[count+1]);
                                    }
                                }
                                // XXX assert type, ref, role are set
                                m_rml_builder->add_member(char_to_item_type(type), ref, role);
                            } else {
                                check_tag(m_relation_builder, element, attrs);
                            }
                            break;
                        case context::in_object:
                            // fallthrough
                        default:
                            assert(false); // should never be here
                    }
                } catch (osmium::memory::BufferIsFull&) {
                    std::cerr << "BUFFER FULL (start_element)" << std::endl;
                    exit(1);
                }
            }

            void close_tl_builder() {
                if (m_tl_builder) {
                    m_tl_builder->add_padding();
                    delete m_tl_builder;
                    m_tl_builder = nullptr;
                }
            }

            void close_wnl_builder() {
                if (m_wnl_builder) {
                    m_wnl_builder->add_padding();
                    delete m_wnl_builder;
                    m_wnl_builder = nullptr;
                }
            }

            void close_rml_builder() {
                if (m_rml_builder) {
                    m_rml_builder->add_padding();
                    delete m_rml_builder;
                    m_rml_builder = nullptr;
                }
            }

            void end_element(const XML_Char* element) {
                try {
                    switch (m_context) {
                        case context::root:
                            assert(false); // should never be here
                            break;
                        case context::top:
                            if (!strcmp(element, "osm") || !strcmp(element, "osmChange")) {
                                m_context = context::root;
                                m_queue.push(m_buffer);
                                m_queue.push(osmium::memory::Buffer()); // empty buffer to signify eof
                            } else if (!strcmp(element, "delete")) {
                                m_in_delete_section = false;
                            }
                            break;
                        case context::node:
                            assert(!strcmp(element, "node"));
                            close_tl_builder();
                            m_buffer.commit();
                            delete m_node_builder;
                            m_node_builder = nullptr;
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::way:
                            assert(!strcmp(element, "way"));
                            close_tl_builder();
                            close_wnl_builder();
                            m_buffer.commit();
                            delete m_way_builder;
                            m_way_builder = nullptr;
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::relation:
                            assert(!strcmp(element, "relation"));
                            close_tl_builder();
                            close_rml_builder();
                            m_buffer.commit();
                            delete m_relation_builder;
                            m_relation_builder = nullptr;
                            m_context = context::top;
                            flush_buffer();
                            break;
                        case context::in_object:
                            m_context = m_last_context;
                            break;
                        default:
                            assert(false); // should never be here
                    }
                } catch (osmium::memory::BufferIsFull&) {
                    std::cerr << "BUFFER FULL (end_element)" << std::endl;
                    exit(1);
                }
            }

            void flush_buffer() {
                if (m_buffer.size() - m_buffer.committed() < 1000 * 1000) {
                    m_queue.push(m_buffer);
                    osmium::memory::Buffer buffer(new char[buffer_size], buffer_size, 0);
                    std::swap(m_buffer, buffer);

                    while (m_queue.size() > m_max_queue_size) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }

        }; // XMLParser

        class XMLInput : public osmium::io::Input {

            static constexpr size_t m_max_queue_size = 100;

            osmium::thread::Queue<osmium::memory::Buffer> m_queue;
            std::atomic<bool> m_done;
            std::thread m_reader;
            std::promise<osmium::io::Meta> m_meta_promise;

        public:

            /**
             * Instantiate XML Parser
             *
             * @param file OSMFile instance.
             */
            XMLInput(const OSMFile& file) :
                osmium::io::Input(file),
                m_queue(),
                m_done(false),
                m_reader() {
            }

            ~XMLInput() {
                if (m_reader.joinable()) {
                    m_reader.join();
                }
            }

            osmium::io::Meta read() override {
                XMLParser parser(fd(), m_queue, m_meta_promise);

                m_reader = std::thread(parser);

                // wait for meta
                return m_meta_promise.get_future().get();
            }

            osmium::memory::Buffer next_buffer() override {
                osmium::memory::Buffer buffer;
                if (m_done && m_queue.empty()) {
                    return buffer;
                }
                m_queue.wait_and_pop(buffer);
                return std::move(buffer);
            }

        }; // class XMLInput

        namespace {

            const bool registered_xml_input = osmium::io::InputFactory::instance().register_input_format({
                osmium::io::Encoding::XML(),
                osmium::io::Encoding::XMLgz(),
                osmium::io::Encoding::XMLbz2()
            }, [](const osmium::OSMFile& file) {
                return new osmium::io::XMLInput(file);
            });

        } // anonymous namespace

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_XML_INPUT_HPP
