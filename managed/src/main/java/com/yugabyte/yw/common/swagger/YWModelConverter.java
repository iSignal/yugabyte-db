package com.yugabyte.yw.common.swagger;

import io.swagger.converter.ModelConverter;
import io.swagger.converter.*;
import io.swagger.models.Model;
import io.swagger.models.properties.Property;

import java.lang.annotation.Annotation;
import java.lang.reflect.Type;
import java.util.Iterator;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class YWModelConverter implements ModelConverter {

    public static final Logger LOG = LoggerFactory.getLogger(YWModelConverter.class);

    @Override
    public Property resolveProperty(Type type,
                                    ModelConverterContext context,
                                    Annotation[] annotations,
                                    Iterator<ModelConverter> chain) {
                                        //LOG.info("resolveProperty1  " + type.getTypeName());
                                        String typeName = type.getTypeName();
                                        if (typeName.indexOf("play.mvc") >= 0 || typeName.indexOf("io.ebean") >= 0) {
                                            LOG.info("skipping property " + typeName);
                                            return null;
                                        }

                                        return chain.next().resolveProperty(type, context, annotations, chain); }

    @Override
    public Model resolve(Type type, ModelConverterContext context, Iterator<ModelConverter> chain) {
        String typeName = type.getTypeName();
        if (typeName.indexOf("play.mvc") >= 0 || typeName.indexOf("io.ebean") >= 0) {
            LOG.info("skipping1 " + typeName);
            return null;
        }
        return chain.next().resolve(type, context, chain);
    }
}